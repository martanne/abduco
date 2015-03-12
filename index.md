---
title: abduco a tool for session {at,de}tach support
layout: default
---

# abduco a tool for session {at,de}tach support

[abduco](http://www.brain-dump.org/projects/abduco) provides
session management i.e. it allows programs to be run independently
from their controlling terminal. That is programs can be detached -
run in the background - and then later reattached. Together with
[dvtm](http://www.brain-dump.org/projects/dvtm) it provides a
simpler and cleaner alternative to tmux or screen.

![abduco+dvtm demo](http://www.brain-dump.org/projects/abduco/screencast.gif)

abduco is in many ways very similar to [dtach]("http://dtach.sf.net)
but is a completely independent implementation which is actively maintained,
contains no legacy code, provides a few additional features, has a
cleaner, more robust implementation and is distributed under the
[ISC license](https://raw.githubusercontent.com/martanne/abduco/master/LICENSE)

## News

 * [abduco-0.3](http://www.brain-dump.org/projects/abduco/abduco-0.3.tar.gz)
   [released](http://lists.suckless.org/dev/1502/25557.html) (19.02.2015)
 * [abduco-0.2](http://www.brain-dump.org/projects/abduco/abduco-0.2.tar.gz)
   [released](http://lists.suckless.org/dev/1411/24447.html) (15.11.2014)
 * [abduco-0.1](http://www.brain-dump.org/projects/abduco/abduco-0.1.tar.gz)
   [released](http://lists.suckless.org/dev/1407/22703.html) (05.07.2014)
 * [Initial announcement](http://lists.suckless.org/dev/1403/20372.html)
   on the suckless development mailing list (08.03.2014)

## Download

Either download the latest source tarball
[abduco-0.3.tar.gz](http://www.brain-dump.org/projects/abduco/abduco-0.3.tar.gz)
with sha1sum

    175b2c0eaf2a8b7fb044f1454d018dac4ec31293  abduco-0.3.tar.gz

compile and install it

    $EDITOR config.mk && make && sudo make install

or use one of the distribution provided binary packages:

 * [Debian](https://packages.debian.org/search?keywords=abduco)
 * [Fedora](https://admin.fedoraproject.org/pkgdb/package/abduco/)
 * [Gentoo](http://packages.gentoo.org/package/app-misc/abduco/)
 * [Ubuntu](http://packages.ubuntu.com/search?keywords=abduco)
 * [Mac OS X](http://www.braumeister.org/formula/abduco) via homebrew

## Quickstart

In order to create a new session `abduco` requires a session name
as well as an command which will be run. If no command is given
the environment variable `$ABDUCO_CMD` is examined and if not set
`dvtm` is executed. Therefore assuming `dvtm` is located somewhere
in `$PATH` a new session named *demo* is created with: 

    $ abduco -c demo

An arbitrary application can be started as follows:

    $ abduco -c session-name your-application

`CTRL-\` detaches from the active session. This detach key can be
changed by means of the `-e` command line option, `-e ^q` would 
for example set it to `CTRL-q`.

To get an overview of existing session run `abduco` without any
arguments.

    $ abduco
    Active sessions (on host debbook)
    * Thu    2015-03-12 12:05:20    demo-active
    + Thu    2015-03-12 12:04:50    demo-finished
      Thu    2015-03-12 12:03:30    demo

A leading asterisk `*` indicates that at least one client is
connected. A leading plus `+` denotes that the session terminated,
attaching to it will print its exit status.

A session can be reattached by using the `-a` command line option
in combination with the session name which was used during session
creation.

    abduco -a demo

Check out the manual page for further information and all available
command line options.

## Improvements over dtach

 * **session list**, available by executing `abduco` without any arguments,
   indicating whether clients are connected or the command has already
   terminated.

 * the **session exit status** of the command being run is always kept and
   reported either upon command termination or on reconnection
   e.g. the following works:

        $ abduco -n demo true && abduco -a demo
        abduco: demo: session terminated with exit status 0

 * **read only sessions** if the `-r` command line argument is used when
   attaching to a session, then all keyboard input is ignored and the
   client is a passive observer only.

 * **better resize handling** on shared sessions, resize request are only
   processed if they are initiated by the most recently connected, non 
   read only client.

 * **socket recreation** by sending the `SIGUSR1` signal to the server 
   process. In case the unix domain socket was removed by accident it
   can be recreated. The simplest way to find out the server process
   id is to look for abduco processes which are reparented to the init
   process.

        $ pgrep -P 1 abduco

   After finding the correct PID the socket can be recreated with

        $ kill -USR1 $PID

   If the abduco binary itself has also been deleted, but a session is 
   still running, use the following command to bring back the session:

        $ /proc/$PID/exe

 * **improved socket permissions** the session sockets are by default either
   stored in `$HOME/.abduco` or `/tmp/abduco/$USER` in both cases it is
   made sure that only the owner has access to the respective directory.

## Development

You can always fetch the current code base from the git repository.

    git clone https://github.com/martanne/abduco.git

or

    git clone git://repo.or.cz/abduco.git

If you have comments, suggestions, ideas, a bug report, a patch or something
else related to abduco then write to the 
[suckless developer mailing list](http://suckless.org/community) 
or contact me directly mat[at]brain-dump.org.

[![Build Status](https://travis-ci.org/martanne/abduco.svg?branch=master)](https://travis-ci.org/martanne/abduco)
[![Coverity Scan Build Status](https://scan.coverity.com/projects/4285/badge.svg)](https://scan.coverity.com/projects/4285)

## License

abduco is licensed under the [ISC license](https://raw.githubusercontent.com/martanne/abduco/master/LICENSE)
