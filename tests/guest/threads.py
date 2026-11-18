import threading
total = [0]
lock = threading.Lock()
def work(n):
    for i in range(2000):
        lock.acquire()
        total[0] += n
        lock.release()
threads = [threading.Thread(target=work, args=(k,)) for k in range(1, 5)]
for t in threads: t.start()
for t in threads: t.join()
print "python threads total", total[0]
