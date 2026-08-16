#!/bin/sh
# Stand-in for ssh in tests. Ignores every option and the destination and runs
# the remote command -- the last argument -- through /bin/sh on this host, the
# way sshd runs it through the login shell. AMREXPLORER_FAKE_SSH_MODE selects
# a misbehaviour:
#   noise   two banner lines on stdout first, as a chatty login shell prints
#   silent  a ready line, then nothing forever, as a wedged server would
#   fail    exit before running anything, as ssh does when it cannot connect
for last; do :; done
case "${AMREXPLORER_FAKE_SSH_MODE:-}" in
    noise)
        echo "Welcome to the fake login node"
        echo "motd: nothing to see here"
        ;;
    silent)
        echo "AMREXPLORER-STDIO 1 TOKEN deadbeef"
        exec sleep 600
        ;;
    fail)
        echo "fake ssh: connect to host nowhere port 22: Connection refused" >&2
        exit 255
        ;;
esac
exec /bin/sh -c "$last"
