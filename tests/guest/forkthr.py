# Forks from a thread while two other threads are busy. In the child only the
# forking thread exists, and its exit status has to reach the parent.
#
# The child leaves with os._exit on purpose. Letting the thread return instead
# runs Python's thread cleanup on locks another thread was waiting for when
# the fork happened, which Leopard's own C library does not survive.
import threading, os
done = False
results = []
def spin():
    x = 0
    while not done:
        x += 1
def forker(code):
    pid = os.fork()
    if pid == 0:
        os._exit(code)
    results.append(os.waitpid(pid, 0)[1] >> 8)
spinners = [threading.Thread(target=spin) for i in range(2)]
for s in spinners: s.start()
for code in (7, 0, 9):
    t = threading.Thread(target=forker, args=(code,))
    t.start(); t.join()
done = True
for s in spinners: s.join()
print "fork from threads:", results
