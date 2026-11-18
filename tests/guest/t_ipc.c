/* Mach messages between two processes, through Apple's own C library.
 *
 * Like t_threads this is a normal dynamically linked Mac OS X program, so it
 * needs a system root to run. A server publishes a port with launchd and
 * forks a client. The client looks the port up and sends a request carrying
 * a number, a port right of its own and a block of out-of-line memory. The
 * server checks all three, calls back on the port it was given, replies, and
 * finally hears from the kernel that the client's port died with the client.
 *
 * Only the server prints; the client reports through its exit status. */
typedef unsigned int u32;

typedef struct {
  u32 bits, size, remote, local, reserved;
  int id;
} header_t;
typedef struct {
  u32 name, pad1;
  unsigned short pad2;
  unsigned char disposition, type;
} port_desc_t;
typedef struct {
  void* address;
  u32 size;
  unsigned char deallocate, copy, pad1, type;
} ool_desc_t;

extern u32 mach_task_self_;
extern u32 bootstrap_port;
extern int mach_msg(header_t*, int option, u32 send_size, u32 rcv_size, u32 rcv_name, u32 timeout,
                    u32 notify);
extern int mach_port_allocate(u32 task, int right, u32* name);
extern int mach_port_insert_right(u32 task, u32 name, u32 right, int kind);
extern int mach_port_request_notification(u32 task, u32 name, int which, u32 sync, u32 notify,
                                          int kind, u32* previous);
extern int bootstrap_register(u32 bootstrap, char* name, u32 port);
extern int bootstrap_look_up(u32 bootstrap, char* name, u32* port);
extern int fork(void);
extern int waitpid(int, int*, int);
extern int printf(const char*, ...);
extern void exit(int);
extern void _exit(int);

/* See t_threads.c for why library functions are called through pointers. */
__asm__(".globl dyld_stub_binding_helper\ndyld_stub_binding_helper:\n hlt\n");
static int (*volatile msg)(header_t*, int, u32, u32, u32, u32, u32) = mach_msg;
static int (*volatile port_allocate)(u32, int, u32*) = mach_port_allocate;
static int (*volatile insert_right)(u32, u32, u32, int) = mach_port_insert_right;
static int (*volatile request_notification)(u32, u32, int, u32, u32, int, u32*) =
    mach_port_request_notification;
static int (*volatile bs_register)(u32, char*, u32) = bootstrap_register;
static int (*volatile bs_look_up)(u32, char*, u32*) = bootstrap_look_up;
static int (*volatile do_fork)(void) = fork;
static int (*volatile do_waitpid)(int, int*, int) = waitpid;
static int (*volatile print)(const char*, ...) = printf;
static void (*volatile quit)(int) = exit;
/* The client leaves without flushing the output buffer it inherited. */
static void (*volatile leave)(int) = _exit;

enum { SEND = 1, RCV = 2, RCV_LARGE = 4, RCV_TIMEOUT = 0x100, AUDIT_TRAILER = 0x03000000 };
enum { MOVE_SEND_ONCE = 18, COPY_SEND = 19, MAKE_SEND = 20, MAKE_SEND_ONCE = 21 };
enum { COMPLEX = (int)0x80000000 };
enum { RECEIVE_RIGHT = 1, DEAD_NAME_NOTICE = 72 };

#define BLOCK 20000
static unsigned char block[BLOCK];

struct request {
  header_t h;
  u32 count;
  port_desc_t port;
  ool_desc_t data;
  u32 ndr[2];
  u32 value;
};
struct small {
  header_t h;
  u32 ndr[2];
  u32 value;
};

static u32 checksum(const unsigned char* p, u32 n) {
  u32 sum = 0;
  for (u32 i = 0; i < n; i++) sum = sum * 31 + p[i];
  return sum;
}

static char service_name[] = "cocaine.test.ipc";

static void client(void) {
  u32 server = 0, callback = 0, reply = 0;
  if (bs_look_up(bootstrap_port, service_name, &server)) leave(10);
  port_allocate(mach_task_self_, RECEIVE_RIGHT, &callback);
  port_allocate(mach_task_self_, RECEIVE_RIGHT, &reply);

  struct request rq = {{0}};
  rq.h.bits = COMPLEX | COPY_SEND | (MAKE_SEND_ONCE << 8);
  rq.h.size = sizeof rq;
  rq.h.remote = server;
  rq.h.local = reply;
  rq.h.id = 7001;
  rq.count = 2;
  rq.port.name = callback;
  rq.port.disposition = MAKE_SEND;
  rq.data.address = block;
  rq.data.size = BLOCK;
  rq.data.copy = 1;
  rq.data.type = 1;
  rq.value = 0x12345678;
  if (msg(&rq.h, SEND, sizeof rq, 0, 0, 0, 0)) leave(11);

  struct {
    struct small m;
    u32 trailer[4];
  } in;
  if (msg(&in.m.h, RCV | RCV_TIMEOUT, 0, sizeof in, callback, 5000, 0)) leave(12);
  if (in.m.h.id != 7002 || in.m.value != 77) leave(13);
  if (msg(&in.m.h, RCV | RCV_TIMEOUT, 0, sizeof in, reply, 5000, 0)) leave(14);
  if (in.m.h.id != 7101 || in.m.value != checksum(block, BLOCK)) leave(15);
  leave(3); /* not zero, so it is seen to travel */
}

int main(void) {
  const u32 task = mach_task_self_;
  u32 service = 0, notify = 0, previous = 0;
  for (u32 i = 0; i < BLOCK; i++) block[i] = (unsigned char)(i * 7 + 3);

  port_allocate(task, RECEIVE_RIGHT, &service);
  insert_right(task, service, service, MAKE_SEND);
  print("register: %d\n", bs_register(bootstrap_port, service_name, service));
  const int child = do_fork();
  if (child == 0) client();

  /* A buffer too small first: with "large" asked for, the message waits. */
  struct {
    header_t h;
    u32 room[2];
  } tiny;
  const int too_large = msg(&tiny.h, RCV | RCV_LARGE | RCV_TIMEOUT, 0, sizeof tiny, service, 5000, 0);
  print("too small a buffer: 0x%x, message needs %u bytes\n", too_large, tiny.h.size);

  struct {
    struct request r;
    u32 trailer[13];
  } rq;
  const int got = msg(&rq.r.h, RCV | RCV_TIMEOUT | AUDIT_TRAILER, 0, sizeof rq, service, 5000, 0);
  print("request: %d, number %d, value 0x%x, %u descriptors\n", got, rq.r.h.id, rq.r.value,
        rq.r.count);
  print("out of line: %u bytes, %s\n", rq.r.data.size,
        rq.r.data.size == BLOCK && checksum(rq.r.data.address, BLOCK) == checksum(block, BLOCK)
            ? "intact"
            : "damaged");
  print("sender is the child: %s\n", rq.trailer[2 + 1 + 2 + 5] == (u32)child ? "yes" : "no");

  /* Call back on the port that came with the request, and watch that port. */
  const u32 callback = rq.r.port.name;
  struct small m = {{0}};
  m.h.bits = COPY_SEND;
  m.h.size = sizeof m;
  m.h.remote = callback;
  m.h.id = 7002;
  m.value = 77;
  print("callback: %d\n", msg(&m.h, SEND, sizeof m, 0, 0, 0, 0));
  port_allocate(task, RECEIVE_RIGHT, &notify);
  print("watch: %d\n", request_notification(task, callback, DEAD_NAME_NOTICE, 0, notify,
                                           MAKE_SEND_ONCE, &previous));

  m.h.bits = MOVE_SEND_ONCE;
  m.h.remote = rq.r.h.remote;
  m.h.id = 7101;
  m.value = checksum(rq.r.data.address, BLOCK);
  print("reply: %d\n", msg(&m.h, SEND, sizeof m, 0, 0, 0, 0));

  int status = 0;
  do_waitpid(child, &status, 0);
  print("client exit status: %d\n", (status >> 8) & 0xFF);

  struct {
    struct small m;
    u32 trailer[4];
  } note;
  const int heard = msg(&note.m.h, RCV | RCV_TIMEOUT, 0, sizeof note, notify, 5000, 0);
  print("port died: %d, notice %d, about the callback port: %s\n", heard, note.m.h.id,
        note.m.value == callback ? "yes" : "no");
  quit(0);
  return 0;
}
