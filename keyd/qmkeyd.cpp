/*!
 * @file qmkeyd.cpp
 * @brief QmKeyd

   <p>
   Copyright (C) 2009-2010 Nokia Corporation

   @author Timo Olkkonen <ext-timo.p.olkkonen@nokia.com>
   @author Yang Yang <ext-yang.25.yang@nokia.com>
   @author Matias Muhonen <ext-matias.muhonen@nokia.com>

   This file is part of SystemSW QtAPI.

   SystemSW QtAPI is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License
   version 2.1 as published by the Free Software Foundation.

   SystemSW QtAPI is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with SystemSW QtAPI.  If not, see <http://www.gnu.org/licenses/>.
   </p>
 */

#include "qmkeyd.h"

#include <fcntl.h>
#include <syslog.h>
#include <errno.h>

#include <sys/inotify.h>
#include <sys/socket.h>

#include <QFile>

#define GPIO_KEYS "/dev/input/gpio-keys"
#define KEYPAD "/dev/input/keypad"
#define ECI "/dev/input/eci"
#define PWRBUTTON "/dev/input/pwrbutton"

#ifndef KEY_CAMERA_FOCUS
#define KEY_CAMERA_FOCUS 0x210
#endif
#ifndef SW_KEYPAD_SLIDE
#define SW_KEYPAD_SLIDE 0x0a
#endif

#define BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define test_bit(bit, array)	((array[LONG(bit)] >> OFF(bit)) & 1)

/*
 * lea  --  helper for address + offset calculations
 */
static inline void *lea(void *base, int offs)
{
    return ((char *)base) + offs;
}

static int  debugmode = 0;

QmKeyd::QmKeyd(int argc, char**argv) : QCoreApplication(argc, argv),
    server(0),
    connections(0),
    gpioFile(-1), keypadFile(-1), eciFile(-1), powerButtonFile(-1), btFile(-1),
    gpioNotifier(0), keypadNotifier(0), eciNotifier(0), powerButtonNotifier(0), btNotifier(0), inputNotifier(0),
    inotifyWd(-1), inotifyFd(-1),
    users(0)
{
    openlog("qmkeyd", LOG_NDELAY|LOG_PID, LOG_DAEMON);

    if (argc > 1 && !strcmp(argv[1], "-d"))
        debugmode = 1;

    server = new QLocalServer(this);
    if (!connect(server, SIGNAL(newConnection()), this, SLOT(newConnection()))) {
        failStart("Failed to connect the newConnection signal\n");
    }

    cleanSocket();

    if (!server->listen(SERVER_NAME)) {
        failStart("Failed to listen incoming connections on %s\n", SERVER_NAME);
    }

    if (chmod(SERVER_NAME, S_IRWXU|S_IRWXG|S_IRWXO) != 0) {
        failStart("Could not set permissions %s\n", SERVER_NAME);
    }

    inotifyFd = inotify_init();
    if (inotifyFd < 0) {
        failStart("Could not create inotify watch for /dev/input\n");
    }

    inotifyWd = inotify_add_watch(inotifyFd, "/dev/input", IN_CREATE | IN_DELETE);
    inputNotifier = new QSocketNotifier(inotifyFd, QSocketNotifier::Read);
    if (!connect(inputNotifier, SIGNAL(activated(int)), this, SLOT(detectBT(int)))) {
        failStart("Failed to connect the inotify activated signal\n");
    }

    /* Make sure btfname is a null-terminated string */
    *btfname = '\0';
}

QmKeyd::~QmKeyd()
{
    server->close();
    delete server, server = 0;
    closelog();

    removeInotifyWatch();
    closeHandles();
    closeBT();
}

void QmKeyd::failStart(const char *fmt, ...)
{
    va_list ap;
    syslog(LOG_CRIT, "qmkeyd start failed, exit\n");

    va_start(ap, fmt);
    syslog(LOG_CRIT, fmt, ap);
    va_end(ap);

    cleanSocket();
    QCoreApplication::exit(1);
}

/* Check if the newly created device is BT headset, if not return false */
bool QmKeyd::isHeadset(int fd)
{
    bool headsetDetected = false;
    unsigned long events[NBITS(EV_MAX)];
    unsigned long bits[NBITS(KEY_MAX)];

    if (ioctl(fd, EVIOCGBIT(0, EV_MAX), events) == -1) {
        goto PROBING_DONE;
    }

    if (!(test_bit(EV_SYN, events) && test_bit(EV_REL, events) && test_bit(EV_REP, events))) {
        goto PROBING_DONE;
    }

    if (test_bit(EV_KEY, events)) {
        if (ioctl(fd, EVIOCGBIT(EV_KEY, KEY_MAX), bits) == -1) {
            goto PROBING_DONE;
        }
        if (test_bit(KEY_PAUSECD, bits) && test_bit(KEY_PLAYCD, bits) &&
            test_bit(KEY_STOPCD, bits) && test_bit(KEY_NEXTSONG, bits) &&
            test_bit(KEY_FASTFORWARD, bits) && test_bit(KEY_PREVIOUSSONG, bits) &&
            test_bit(KEY_REWIND, bits)) {
            headsetDetected = true;
        }
    }

PROBING_DONE:
    return headsetDetected;
}

void QmKeyd::cleanSocket()
{
    QFile serverSocket(SERVER_NAME);
    if (serverSocket.exists()) {
        /* If a socket exists but we fail to delete it, it can be a sign of a potential
         * race condition. Therefore, exit the process as it is a critical failure.
         */
        if (!serverSocket.remove()) {
            syslog(LOG_CRIT, "Could not clean the existing socket %s, exit\n", SERVER_NAME);
            QCoreApplication::exit(1);
        }
    }
}

void QmKeyd::detectBT(int inotify)
{
    int fd;
    char fname[32];
    char buf[2<<10];
    struct inotify_event *ev;

    int n = read(inotify, buf, sizeof buf);

    if (n == -1) {
        if (errno != EINTR) {
            syslog(LOG_WARNING, "inotify read: %s\n", strerror(errno));
            removeInotifyWatch();
        }
    } else if (n < (int)sizeof *ev) {
        syslog(LOG_WARNING, "inotify read: %d / %Zd\n", n, sizeof *ev);
        removeInotifyWatch();
    } else {
        ev = (inotify_event *)lea(buf, 0);
        while (n >= (int)sizeof *ev) {
            int k = sizeof *ev + ev->len;

            if ((k < (int)sizeof *ev) || (k > n)) {
                break;
            }

            snprintf(fname, sizeof(fname), "/dev/input/%s", ev->name);

            switch (ev->mask) {
                case IN_DELETE:
                     /* Check if the current headset was disconnected */
                     if (btFile != -1 && strncmp(btfname, fname, sizeof(btfname)) == 0) {
                         closeBT();
                     }
                     break;

                case IN_CREATE:
                     /* Check if a headset was connected */
                     if ((fd = open(fname, O_RDONLY | O_NONBLOCK)) == -1) {
                         syslog(LOG_WARNING, "Could not open %s for detecting a headset\n", fname);
                         continue;
                     }

                     if (isHeadset(fd)) {
                         /* Yes, found a new headset. Close an existing headset, if there's one */
                         closeBT();

                         strncpy(btfname, fname, sizeof(btfname));
                         btFile = fd;

                         /* Receive notifications for the headset */
                         btNotifier = new QSocketNotifier(btFile, QSocketNotifier::Read);
                         connect(btNotifier, SIGNAL(activated(int)), this, SLOT(readyRead(int)));
                     } else {
                         close(fd);
                     }
                     break;
            }

            n -= k;
            ev = (inotify_event *)lea(ev, k);
        }
    }
}

void QmKeyd::newConnection()
{
    while (server->hasPendingConnections()) {
        QLocalSocket *socket = server->nextPendingConnection();
        connect(socket, SIGNAL(disconnected()), this, SLOT(disconnected()));
        connect(socket, SIGNAL(readyRead()), this, SLOT(socketReadyRead()));
        connections.push_back(socket);
        users++;

        if (debugmode) {

            struct ucred cr;
            socklen_t    cl = sizeof(cr);
            pid_t        pid = 0;
            int          fd = socket->socketDescriptor();

            if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0) {
                pid = cr.pid;
                // printf("Peer's pid=%d, uid=%d, gid=%d\n", cr.pid, cr.uid, cr.gid);
            }

            syslog(LOG_DEBUG, "New client with PID %u, socket %p, clients now %d\n", (unsigned int)pid, socket, users);
        }

        if (users >= 1)
            openHandles();
    }
}

void QmKeyd::disconnected()
{
    QLocalSocket *socket = qobject_cast<QLocalSocket*>(sender());
    for (QVector<QLocalSocket*>::iterator it = connections.begin(); it != connections.end(); it++) {
        if (*it == socket) {

            users--;

            if (debugmode) {
                syslog(LOG_DEBUG, "Client with socket %p disappeared, clients now %d\n", socket, users);
            }

            connections.erase(it);

            if (!users)
                closeHandles();
            break;
        }
    }
    socket->deleteLater();
}

void QmKeyd::socketReadyRead() {
    QLocalSocket *socket = qobject_cast<QLocalSocket*>(sender());
    if (!socket) {
        syslog(LOG_WARNING, "An invalid socket to be read\n");
        return;
    }
    struct input_event ev;
    int ret = socket->read((char*)&ev, sizeof(ev));
    if (ret == sizeof(ev)) {
        QList<int> fdList = readFrom(ev);
        int fd;
        /*The EVIOCGKEY ioctl is used to determine the global key and button state for a device.
         *EVIOCGKEY sets a bit in the bit array for each key or button that is pressed.
         */
        foreach (fd, fdList) {
            if (fd != -1 && ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) != -1) {
                if (((keys[ev.code/8]) & (1 << (ev.code % 8))) != 0) {
                    ev.value = 1;
                    break;
                } else {
                    ev.value = 0;
                }
            } else {
                ev.value = 0;
            }
        }
        if (socket->write((char*)&ev, sizeof(ev)) != sizeof(ev)) {
            int          fd = socket->socketDescriptor();
            syslog(LOG_WARNING, "Could not write to a socket %d\n", fd);
        }
    }
}

void QmKeyd::readyRead(int fd)
{
    int ret = 0;
    do {
        struct input_event ev;
        ret = read(fd, &ev, sizeof(ev));
        if (ret == sizeof(ev) && !readFrom(ev).isEmpty()) {
            QLocalSocket *socket;
            foreach (socket, connections) {
                socket->write((char*)&ev, sizeof(ev));
            }
        }
    } while (ret > 0);
}

QList<int> QmKeyd::readFrom(struct input_event &ev)
{
    QList<int> list;
    if (ev.type == EV_KEY) {
        switch (ev.code) {
        case KEY_RIGHTCTRL:
            list.push_back(keypadFile);
            break;
        case KEY_CAMERA:
        case KEY_CAMERA_FOCUS:
            list.push_back(gpioFile);
            break;
        case KEY_VOLUMEUP:
        case KEY_VOLUMEDOWN:
            list.push_back(keypadFile);
            list.push_back(eciFile);
            list.push_back(btFile);
            break;
        case KEY_UP:
        case KEY_LEFT:
        case KEY_RIGHT:
        case KEY_END:
        case KEY_DOWN:
        case KEY_MUTE:
        case KEY_STOP:
        case KEY_FORWARD:
        case KEY_PLAYPAUSE:
        case KEY_PHONE:
            list.push_back(eciFile);
            break;
        case KEY_PAUSECD:
        case KEY_PLAYCD:
        case KEY_STOPCD:
        case KEY_NEXTSONG:
        case KEY_FASTFORWARD:
        case KEY_PREVIOUSSONG:
            list.push_back(btFile);
            break;
        case KEY_REWIND:
            list.push_back(btFile);
            list.push_back(eciFile);
            break;
        case KEY_POWER:
            list.push_back(powerButtonFile);
            break;
        }
    } else if (ev.type == EV_SW) {
        switch (ev.code) {
        case SW_KEYPAD_SLIDE:
            list.push_back(gpioFile);
            break;
        }
    }
    return list;
}

void QmKeyd::openHandles()
{
    if (gpioFile == -1) {
        gpioFile = open(GPIO_KEYS, O_RDONLY | O_NONBLOCK);
        if (gpioFile != -1) {
            gpioNotifier = new QSocketNotifier(gpioFile, QSocketNotifier::Read);
            connect(gpioNotifier, SIGNAL(activated(int)), this, SLOT(readyRead(int)));
        } else {
            syslog(LOG_WARNING, "Could not open %s\n", GPIO_KEYS);
            gpioNotifier = 0;
        }
    }

    if (keypadFile == -1) {
        keypadFile = open(KEYPAD, O_RDONLY | O_NONBLOCK);
        if (keypadFile != -1) {
            keypadNotifier = new QSocketNotifier(keypadFile, QSocketNotifier::Read);
            connect(keypadNotifier, SIGNAL(activated(int)), this, SLOT(readyRead(int)));
        } else {
            syslog(LOG_WARNING, "Could not open %s\n", KEYPAD);
            keypadNotifier = 0;
        }
    }
    if (eciFile == -1) {
        eciFile = open(ECI, O_RDONLY | O_NONBLOCK);
        if (eciFile != -1) {
            eciNotifier = new QSocketNotifier(eciFile, QSocketNotifier::Read);
            connect(eciNotifier, SIGNAL(activated(int)), this, SLOT(readyRead(int)));
        } else {
            syslog(LOG_WARNING, "Could not open %s\n", ECI);
            eciNotifier = 0;
        }
    }
    if (powerButtonFile == -1) {
        powerButtonFile = open(PWRBUTTON, O_RDONLY | O_NONBLOCK);
        if (powerButtonFile != -1) {
            powerButtonNotifier = new QSocketNotifier(powerButtonFile, QSocketNotifier::Read);
            connect(powerButtonNotifier, SIGNAL(activated(int)), this, SLOT(readyRead(int)));
        } else {
            syslog(LOG_WARNING, "Could not open %s\n", PWRBUTTON);
            powerButtonNotifier = 0;
        }
    }
}

void QmKeyd::closeHandles()
{
    if (gpioFile != -1) {
        close(gpioFile), gpioFile = -1;
        delete gpioNotifier, gpioNotifier = 0;
    }
    if (keypadFile != -1) {
        close(keypadFile), keypadFile = -1;
        delete keypadNotifier, keypadNotifier = 0;
    }
    if (eciFile != -1) {
        close(eciFile), eciFile = -1;
        delete eciNotifier, eciNotifier = 0;
    }
    if (powerButtonFile != -1) {
        close(powerButtonFile), powerButtonFile = -1;
        delete powerButtonNotifier, powerButtonNotifier = 0;
    }
}

void QmKeyd::closeBT()
{
    if (btNotifier) {
        delete btNotifier, btNotifier = 0;
    }
    if (btFile != -1) {
        close(btFile), btFile = -1;
    }
}

void QmKeyd::removeInotifyWatch()
{
    if (inputNotifier) {
        delete inputNotifier, inputNotifier = 0;
    }
    if (inotifyFd != -1) {
        inotify_rm_watch(inotifyFd, inotifyWd);
        close(inotifyFd), inotifyFd = -1;
    }
}
