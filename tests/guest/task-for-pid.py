import ctypes
import os

lib = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
port = ctypes.c_uint
lib.task_for_pid.argtypes = [port, ctypes.c_int, ctypes.POINTER(port)]
lib.pid_for_task.argtypes = [port, ctypes.POINTER(ctypes.c_int)]
lib.mach_port_deallocate.argtypes = [port, port]
self_task = port.in_dll(lib, "mach_task_self_").value

def lookup(pid, target=self_task):
    task = port(123)
    result = lib.task_for_pid(target, pid, ctypes.byref(task))
    return result, task.value

def check_pid(task, pid):
    got = ctypes.c_int()
    assert lib.pid_for_task(task, ctypes.byref(got)) == 0
    assert got.value == pid

result, task = lookup(os.getpid())
assert result == 0
check_pid(task, os.getpid())
assert lib.mach_port_deallocate(self_task, task) == 0
assert lookup(os.getpid(), 0)[0] != 0
assert lookup(1)[0] != 0
assert lookup(os.getppid())[0] != 0
assert lib.task_for_pid(self_task, os.getpid(), None) != 0

ready_r, ready_w = os.pipe()
done_r, done_w = os.pipe()
child = os.fork()
if child == 0:
    os.close(ready_r)
    os.close(done_w)
    child_task = port.in_dll(lib, "mach_task_self_").value
    result, task = lookup(os.getppid(), child_task)
    assert result == 0
    check_pid(task, os.getppid())
    assert lib.mach_port_deallocate(child_task, task) == 0
    os.write(ready_w, "x")
    os.read(done_r, 1)
    os._exit(0)

os.close(ready_w)
os.close(done_r)
result, task = lookup(child)
assert result == 0
check_pid(task, child)
assert lib.mach_port_deallocate(self_task, task) == 0
assert os.read(ready_r, 1) == "x"
result, task = lookup(child)
assert result == 0
check_pid(task, child)
assert lib.mach_port_deallocate(self_task, task) == 0
os.write(done_w, "x")
os.waitpid(child, 0)
result, task = lookup(child)
assert result != 0 and task == 0

ready_r, ready_w = os.pipe()
done_r, done_w = os.pipe()
child = os.fork()
if child == 0:
    os.close(ready_r)
    os.close(done_w)
    code = "import os; os.write(%d, 'x'); os.read(%d, 1)" % (ready_w, done_r)
    os.execl("/usr/bin/python2.5", "python2.5", "-c", code)
    os._exit(1)
os.close(ready_w)
os.close(done_r)
assert os.read(ready_r, 1) == "x"
result, task = lookup(child)
assert result == 0
check_pid(task, child)
assert lib.mach_port_deallocate(self_task, task) == 0
os.write(done_w, "x")
os.waitpid(child, 0)
assert lookup(child)[0] != 0
print "task_for_pid ok"
