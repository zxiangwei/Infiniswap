/*
 * Infiniswap, remote memory paging over RDMA
 * Copyright 2017 University of Michigan, Ann Arbor
 * GPLv2 License
 */

#include "rdma-common.h"

static int on_connect_request(struct rdma_cm_id *id);
static int on_connection(struct rdma_cm_id *id);
static int on_disconnect(struct rdma_cm_id *id);
static int on_event(struct rdma_cm_event *event);
static void usage(const char *argv0);
long page_size;
int running;
//创建RDMA事件通道，监听客户端连接请求
int main(int argc, char **argv)
{
  struct sockaddr_in6 addr;
  struct rdma_cm_event *event = NULL;
  struct rdma_cm_id *listener = NULL;
  struct rdma_event_channel *ec = NULL;
  uint16_t port = 0;
  pthread_t free_mem_thread;//释放内存的线程

  if (argc != 3)
    usage(argv[0]);
  page_size = sysconf(_SC_PAGE_SIZE);

  memset(&addr, 0, sizeof(addr));//初始化为0
  addr.sin6_family = AF_INET6;//IPV6
  /*
  inet_pton函数用于将字符串类型的IP地址转换为网络字节序的二进制形式，并存储在addr.sin6_addr中。
  argv[1]表示输入的IP地址，&addr.sin6_addr表示存储地址的指针。
  */
  inet_pton(AF_INET6, argv[1], &addr.sin6_addr);
  addr.sin6_port = htons(atoi(argv[2]));//端口号

  TEST_Z(ec = rdma_create_event_channel());
  TEST_NZ(rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP));
  TEST_NZ(rdma_bind_addr(listener, (struct sockaddr *)&addr));
  TEST_NZ(rdma_listen(listener, 10)); /* backlog=10 is arbitrary */

  port = ntohs(rdma_get_src_port(listener));

  printf("listening on port %d.\n", port);

  //free
  running = 1;
  //创建一个线程，用于释放内存。创建失败则退出程序
  /*
    其中，&free_mem_thread表示新线程的ID，
    NULL表示线程的属性，
    (void *)free_mem表示线程的入口函数，
    NULL表示传递给线程入口函数的参数。
  */
  TEST_NZ(pthread_create(&free_mem_thread, NULL, (void *)free_mem, NULL));

  //事件处理，没有事件则退出循环
  while (rdma_get_cm_event(ec, &event) == 0) {//从RDMA事件通道中获取事件，如果获取事件，则返回0
    struct rdma_cm_event event_copy;

    //事件复制。避免直接修改event指向的事件数据，从而保证事件数据的完整性。
    memcpy(&event_copy, event, sizeof(*event));
    rdma_ack_cm_event(event);

    if (on_event(&event_copy))//看起来好像没啥意义？on_event的返回值一直都会是0
      break;
  }

  rdma_destroy_id(listener);//销毁RDMA连接
  rdma_destroy_event_channel(ec);//销毁事件通道

  return 0;
}

//连接请求
int on_connect_request(struct rdma_cm_id *id)
{
  struct rdma_conn_param cm_params;

  printf("received connection request.\n");
  build_connection(id);
  build_params(&cm_params);
  TEST_NZ(rdma_accept(id, &cm_params));

  return 0;
}

//建立连接
int on_connection(struct rdma_cm_id *id)
{
  on_connect(id->context);

  printf("connection build\n");
  /* J: only server send mr, client doesn't */
  send_free_mem_size(id->context);

  return 0;
}

//断开连接
int on_disconnect(struct rdma_cm_id *id)
{
  printf("peer disconnected.\n");

  destroy_connection(id->context);
  return 0;
}

//判断事件
int on_event(struct rdma_cm_event *event)
{
  int r = 0;

  if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST)
    r = on_connect_request(event->id);
  else if (event->event == RDMA_CM_EVENT_ESTABLISHED)
    r = on_connection(event->id);
  else if (event->event == RDMA_CM_EVENT_DISCONNECTED)
    r = on_disconnect(event->id);
  else
    die("on_event: unknown event.");

  return r;
}

//使用情况
void usage(const char *argv0)
{
  fprintf(stderr, "usage: %s ip port\n", argv0);
  exit(1);
}
