/*
   sslh-fork: forking server

# Copyright (C) 2007-2012  Yves Rutschle
# 
# This program is free software; you can redistribute it
# and/or modify it under the terms of the GNU General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later
# version.
# 
# This program is distributed in the hope that it will be
# useful, but WITHOUT ANY WARRANTY; without even the implied
# warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
# PURPOSE.  See the GNU General Public License for more
# details.
# 
# The full text for the General Public License is here:
# http://www.gnu.org/licenses/gpl.html

*/

#include <sys/epoll.h>
#include <sys/signalfd.h>


#include "common.h"
#include "probe.h"

const char* server_type = "sslh-fork";

#define MAX(a, b)  (((a) > (b)) ? (a) : (b))

/* shovels data from one fd to the other and vice-versa 
   returns after one socket closed
 */
int shovel(struct connection *cnx)
{
   fd_set fds;
   int res, i;
   int max_fd = MAX(cnx->q[0].fd, cnx->q[1].fd) + 1;

   FD_ZERO(&fds);
   while (1) {
      FD_SET(cnx->q[0].fd, &fds);
      FD_SET(cnx->q[1].fd, &fds);

      res = select(
                   max_fd,
                   &fds,
                   NULL,
                   NULL,
                   NULL
                  );
      CHECK_RES_DIE(res, "select");

      for (i = 0; i < 2; i++) {
          if (FD_ISSET(cnx->q[i].fd, &fds)) {
              res = fd2fd(&cnx->q[1-i], &cnx->q[i]);
              if (!res) {
                  if (verbose) 
                      fprintf(stderr, "%s %s", i ? "client" : "server", "socket closed\n");
                  return res;
              }
          }
      }
   }
}

/* Child process that finds out what to connect to and proxies 
 */
void start_shoveler(int in_socket)
{
   fd_set fds;
   struct timeval tv;
   int res = PROBE_AGAIN;
   int out_socket;
   struct connection cnx;

   init_cnx(&cnx);
   cnx.q[0].fd = in_socket;

   FD_ZERO(&fds);
   FD_SET(in_socket, &fds);
   memset(&tv, 0, sizeof(tv));
   tv.tv_sec = probing_timeout;

   while (res == PROBE_AGAIN) {
       /* POSIX does not guarantee that tv will be updated, but the client can
        * only postpone the inevitable for so long */
       res = select(in_socket + 1, &fds, NULL, NULL, &tv);
       if (res == -1)
           perror("select");

       if (FD_ISSET(in_socket, &fds)) {
           /* Received data: figure out what protocol it is */
           res = probe_client_protocol(&cnx);
       } else {
           /* Timed out: it's necessarily SSH */
           cnx.proto = timeout_protocol();
           break;
       }
   }

   if (cnx.proto->service &&
       check_access_rights(in_socket, cnx.proto->service)) {
       exit(0);
   }

   /* Connect the target socket */
   out_socket = connect_addr(&cnx, in_socket);
   CHECK_RES_DIE(out_socket, "connect");

   cnx.q[1].fd = out_socket;

   log_connection(&cnx);

   flush_deferred(&cnx.q[1]);

   shovel(&cnx);

   close(in_socket);
   close(out_socket);
   
   if (verbose)
      fprintf(stderr, "connection closed down\n");

   exit(0);
}

/* number of children */
static int listener_pid_number = 5;
static int listener_pid[5];

void stop_listeners(int sig)
{
    int i;

    for (i = 0; i < listener_pid_number; i++) {
        kill(listener_pid[i], sig);
    }
}

               /* 0 terminated array */
int main_loop(int listen_sockets[])
{
    int in_socket, i, res, epollfd, r, sfd;
    struct epoll_event ev, events[1];
    struct sigaction action;
    sigset_t mask;

    listener_pid[0] = 0;

    epollfd = epoll_create1(EPOLL_CLOEXEC);
    if (epollfd < 0) {
        fprintf(stderr, "epoll_create1() failed: %m");
        return -errno;
    }

    ev.events = EPOLLIN|EPOLLET;
    /* Add all the listening sockets to epoll */
    for (i = 0; listen_sockets[i]; i++) {
        ev.data.fd = listen_sockets[i];
        r = epoll_ctl(epollfd, EPOLL_CTL_ADD, listen_sockets[i], &ev);
        if (r < 0) {
             fprintf(stderr, "epoll_ctl() failed: %m");
             return -errno;
        }
    }

    /* add signalfd for graceful exit */
    sigemptyset(&mask);
    signaddset(&mask, SIGTERM);
    sfd = signalfd(-1, &mask, 0);
    ev.events = EPOLLIN;
    ev.data.fd = sfd;
    r = epoll_ctl(epollfd, EPOLL_CTL_ADD, sfd, &ev);
    if (r < 0) {
         fprintf(stderr, "epoll_ctl() failed: %m");
         return -errno;
    }

    /* fork() children */
    for (i = 0; i < listener_pid_number; i++)
        if ((listener_pid[i] = fork()) == 0)
            break;

    while (1) {
        struct signalfd_siginfo s;

        r = epoll_wait(epollfd, events, 1, -1);
        if (r < 0) {
            fprintf(stderr, "epoll_wait() failed: %m");
            return -errno;
        }

        if (events[0].data.fd == sfd) {
            r = read(sfd, &s, sizeof(s));
            if (s.ssi_signo == SIGTERM) {
                if (listener_pid[0] > 0) {
                    /* parent */
                    stop_listeners(SIGTERM);
                } else
                    _exit(EXIT_SUCCESS);
            }
            fprintf(stderr, "Shouldn't be here");
            _exit(EXIT_FAILURE);
        }

        in_socket = accept(events[0].data.fd, 0, 0);
        if (verbose) fprintf(stderr, "accepted fd %d from sockfd %d\n", in_socket, events[0].data.fd);

        start_shoveler(in_socket);
        close(in_socket);
    }
}

/* The actual main is in common.c: it's the same for both version of
 * the server
 */

