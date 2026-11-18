# One program waits for a notification that another posts: through Apple's
# directory daemon (to find the user), dirhelper (for the user's temporary
# folder) and notifyd. The poster keeps posting until the waiter has heard.
/usr/bin/notifyutil -1 cocaine.ping &
w=$!
while kill -0 $w 2>/dev/null; do
  /bin/sleep 1
  /usr/bin/notifyutil -p cocaine.ping
done
wait $w
echo "waiter finished: $?"
