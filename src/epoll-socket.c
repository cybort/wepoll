#include <assert.h>
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>

#include "afd.h"
#include "epoll-socket.h"
#include "epoll.h"
#include "error.h"
#include "poll-group.h"
#include "poll-request.h"
#include "port.h"

#ifndef SIO_BASE_HANDLE
#define SIO_BASE_HANDLE 0x48000022
#endif

#define _EP_EVENT_MASK 0xffff

enum _poll_status { _POLL_IDLE = 0, _POLL_PENDING, _POLL_CANCELLED };

typedef struct _ep_sock_private {
  ep_sock_t pub;
  SOCKET afd_socket;
  poll_group_t* poll_group;
  poll_req_t* poll_req;
  epoll_data_t user_data;
  uint32_t user_events;
  uint32_t pending_events;
  uint8_t poll_status;
  unsigned deleted : 1;
} _ep_sock_private_t;

static inline _ep_sock_private_t* _ep_sock_private(ep_sock_t* sock_info) {
  return container_of(sock_info, _ep_sock_private_t, pub);
}

static inline _ep_sock_private_t* _ep_sock_alloc(void) {
  _ep_sock_private_t* sock_private = malloc(sizeof *sock_private);
  if (sock_private == NULL)
    return_error(NULL, ERROR_NOT_ENOUGH_MEMORY);
  return sock_private;
}

static inline void _ep_sock_free(_ep_sock_private_t* sock_private) {
  assert(sock_private->poll_status == _POLL_IDLE);
  poll_req_delete(sock_private->poll_req);
  free(sock_private);
}

static int _get_related_sockets(ep_port_t* port_info,
                                SOCKET socket,
                                SOCKET* afd_socket_out,
                                poll_group_t** poll_group_out) {
  SOCKET afd_socket;
  poll_group_t* poll_group;
  DWORD bytes;

  /* Try to obtain a base handle for the socket, so we can bypass LSPs
   * that get in the way if we want to talk to the kernel directly. If
   * it fails we try if we work with the original socket. Note that on
   * windows XP/2k3 this will always fail since they don't support the
   * SIO_BASE_HANDLE ioctl.
   */
  afd_socket = socket;
  WSAIoctl(socket,
           SIO_BASE_HANDLE,
           NULL,
           0,
           &afd_socket,
           sizeof afd_socket,
           &bytes,
           NULL,
           NULL);

  poll_group = ep_port_acquire_poll_group(port_info, afd_socket);
  if (poll_group == NULL)
    return -1;

  *afd_socket_out = afd_socket;
  *poll_group_out = poll_group;

  return 0;
}

static int _ep_sock_set_socket(ep_port_t* port_info,
                               _ep_sock_private_t* sock_private,
                               SOCKET socket) {
  if (socket == 0 || socket == INVALID_SOCKET)
    return_error(-1, ERROR_INVALID_HANDLE);

  assert(sock_private->afd_socket == 0);

  if (_get_related_sockets(port_info,
                           socket,
                           &sock_private->afd_socket,
                           &sock_private->poll_group) < 0)
    return -1;

  if (ep_port_add_socket(port_info, &sock_private->pub.tree_node, socket) < 0)
    return -1;

  return 0;
}

ep_sock_t* ep_sock_new(ep_port_t* port_info, SOCKET socket) {
  _ep_sock_private_t* sock_private = _ep_sock_alloc();
  if (sock_private == NULL)
    return NULL;

  memset(sock_private, 0, sizeof *sock_private);
  tree_node_init(&sock_private->pub.tree_node);
  queue_node_init(&sock_private->pub.queue_node);

  if (_ep_sock_set_socket(port_info, sock_private, socket) < 0) {
    _ep_sock_free(sock_private);
    return NULL;
  }

  poll_req_t* poll_req = poll_req_new(&sock_private->pub);
  assert(poll_req != NULL);
  sock_private->poll_req = poll_req;

  return &sock_private->pub;
}

void _ep_sock_maybe_free(_ep_sock_private_t* sock_private) {
  /* The socket may still have pending overlapped requests that have yet to be
   * reported by the completion port. If that's the case the memory can't be
   * released yet. It'll be released later as ep_sock_unregister_poll_req()
   * calls this function.
   */
  if (sock_private->deleted && sock_private->poll_status == _POLL_IDLE)
    _ep_sock_free(sock_private);
}

void ep_sock_delete(ep_port_t* port_info, ep_sock_t* sock_info) {
  _ep_sock_private_t* sock_private = _ep_sock_private(sock_info);

  assert(!sock_private->deleted);
  sock_private->deleted = true;

  ep_port_del_socket(port_info, &sock_info->tree_node);
  ep_port_clear_socket_update(port_info, sock_info);
  ep_port_release_poll_group(sock_private->poll_group);
  sock_private->poll_group = NULL;

  _ep_sock_maybe_free(sock_private);
}

void ep_sock_force_delete(ep_port_t* port_info, ep_sock_t* sock_info) {
  _ep_sock_private_t* sock_private = _ep_sock_private(sock_info);
  sock_private->poll_status = _POLL_IDLE;
  ep_sock_delete(port_info, sock_info);
}

ep_sock_t* ep_sock_find(tree_t* tree, SOCKET socket) {
  tree_node_t* tree_node = tree_find(tree, socket);
  if (tree_node == NULL)
    return NULL;

  return container_of(tree_node, ep_sock_t, tree_node);
}

int ep_sock_set_event(ep_port_t* port_info,
                      ep_sock_t* sock_info,
                      const struct epoll_event* ev) {
  _ep_sock_private_t* sock_private = _ep_sock_private(sock_info);

  /* EPOLLERR and EPOLLHUP are always reported, even when no sollicited. */
  uint32_t events = ev->events | EPOLLERR | EPOLLHUP;

  sock_private->user_events = events;
  sock_private->user_data = ev->data;

  if ((events & _EP_EVENT_MASK & ~(sock_private->pending_events)) != 0)
    ep_port_request_socket_update(port_info, sock_info);

  return 0;
}

int ep_sock_update(ep_port_t* port_info, ep_sock_t* sock_info) {
  _ep_sock_private_t* sock_private = _ep_sock_private(sock_info);
  SOCKET driver_socket = poll_group_get_socket(sock_private->poll_group);
  bool broken = false;

  assert(ep_port_is_socket_update_pending(port_info, sock_info));

  if (sock_private->poll_status == _POLL_PENDING &&
      (sock_private->user_events & _EP_EVENT_MASK &
       ~sock_private->pending_events) == 0) {
    /* All the events the user is interested in are already being monitored
     * by the pending poll request. It might spuriously complete because of an
     * event that we're no longer interested in; if that happens we just
     * submit another poll request with the right event mask. */

  } else if (sock_private->poll_status == _POLL_PENDING) {
    /* A poll request is already pending, but it's not monitoring for all the
     * events that the user is interested in. Cancel the pending poll request;
     * when it completes it will be submitted again with the correct event
     * mask. */
    if (poll_req_cancel(sock_private->poll_req, driver_socket) < 0)
      return -1;
    sock_private->poll_status = _POLL_CANCELLED;
    sock_private->pending_events = 0;

  } else if (sock_private->poll_status == _POLL_CANCELLED) {
    /* The poll request has already been cancelled, we're still waiting for it
     * to return. For now, there's nothing that needs to be done. */

  } else if (sock_private->poll_status == _POLL_IDLE) {
    if (poll_req_submit(sock_private->poll_req,
                        sock_private->user_events,
                        sock_private->afd_socket,
                        driver_socket) < 0) {
      if (GetLastError() == ERROR_INVALID_HANDLE)
        /* The socket is broken. It will be dropped from the epoll set. */
        broken = true;
      else
        /* Another error occurred, which is propagated to the caller. */
        return -1;

    } else {
      /* The poll request was successfully submitted. */
      sock_private->poll_status = _POLL_PENDING;
      sock_private->pending_events = sock_private->user_events;
    }
  } else {
    /* Unreachable. */
    assert(false);
  }

  ep_port_clear_socket_update(port_info, sock_info);

  /* If we saw an ERROR_INVALID_HANDLE error, drop the socket. */
  if (broken)
    ep_sock_delete(port_info, sock_info);

  return 0;
}

int ep_sock_feed_event(ep_port_t* port_info,
                       poll_req_t* poll_req,
                       struct epoll_event* ev) {
  ep_sock_t* sock_info = poll_req_get_sock_data(poll_req);
  _ep_sock_private_t* sock_private = _ep_sock_private(sock_info);

  uint32_t epoll_events;
  bool drop_socket;
  int ev_count = 0;

  sock_private->poll_status = _POLL_IDLE;
  sock_private->pending_events = 0;

  if (sock_private->deleted) {
    /* Ignore completion for overlapped poll operation if the socket has been
     * deleted; instead, free the socket. */
    _ep_sock_free(sock_private);
    return 0;
  }

  poll_req_complete(poll_req, &epoll_events, &drop_socket);

  /* Filter events that the user didn't ask for. */
  epoll_events &= sock_private->user_events;

  /* Clear the event mask if EPOLLONESHOT is set and there are any events
   * to report. */
  if (epoll_events != 0 && (sock_private->user_events & EPOLLONESHOT))
    sock_private->user_events = EPOLLERR | EPOLLHUP;

  /* Fill the ev structure if there are any events to report. */
  if (epoll_events != 0) {
    ev->data = sock_private->user_data;
    ev->events = epoll_events;
    ev_count = 1;
  }

  if (drop_socket)
    /* Drop the socket from the epoll set. */
    ep_sock_delete(port_info, sock_info);
  else
    /* Put the socket back onto the attention list so a new poll request will
     * be submitted. */
    ep_port_request_socket_update(port_info, sock_info);

  return ev_count;
}
