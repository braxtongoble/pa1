/*
# Copyright 2025 University of Kentucky
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
*/

/*
Please specify the group members here
# Student #1: Braxton Goble
# Student #2:
# Student #3:
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <errno.h>
#include <fcntl.h>

#define MAX_EVENTS 64
#define MESSAGE_SIZE 16
#define DEFAULT_CLIENT_THREADS 4

char *server_ip = "127.0.0.1";
int server_port = 12345;
int num_client_threads = DEFAULT_CLIENT_THREADS;
int num_requests = 1000000;

/*
 * This structure is used to store per-thread data in the client
 */
typedef struct
{
    int epoll_fd;        /* File descriptor for the epoll instance, used for monitoring events on the socket. */
    int socket_fd;       /* File descriptor for the client socket connected to the server. */
    long long total_rtt; /* Accumulated Round-Trip Time (RTT) for all messages sent and received (in microseconds). */
    long total_messages; /* Total number of messages sent and received. */
    float request_rate;  /* Computed request rate (requests per second) based on RTT and total messages. */
} client_thread_data_t;

/*
 * This function runs in a separate client thread to handle communication with the server
 */
void *client_thread_func(void *arg)
{
    client_thread_data_t *data = (client_thread_data_t *)arg;
    struct epoll_event event, events[MAX_EVENTS];
    char send_buf[MESSAGE_SIZE] = "ABCDEFGHIJKMLNOP"; /* Send 16-Bytes message every time */
    char recv_buf[MESSAGE_SIZE];
    struct timeval start, end;
    int remaining_requests = num_requests / num_client_threads;

    // Register the socket with epoll
    event.events = EPOLLIN | EPOLLOUT;
    event.data.fd = data->socket_fd;
    if (epoll_ctl(data->epoll_fd, EPOLL_CTL_ADD, data->socket_fd, &event) == -1)
    {
        perror("epoll_ctl: add");
        close(data->socket_fd);
        close(data->epoll_fd);
        return NULL;
    }

    data->total_rtt = 0;
    data->total_messages = 0;

    int waiting_for_response = 0;

    while (remaining_requests > 0 || waiting_for_response)
    {
        int nfds = epoll_wait(data->epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1)
        {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            if (events[i].data.fd == data->socket_fd)
            {
                // Handle event on the socket
                if (events[i].events & EPOLLIN)
                {
                    // Socket is ready for reading - receive response
                    int bytes_read = recv(data->socket_fd, recv_buf, MESSAGE_SIZE, 0);
                    if (bytes_read <= 0)
                    {
                        if (bytes_read == 0)
                        {
                            // Connection closed
                            printf("Server closed connection\n");
                        }
                        else
                        {
                            perror("recv");
                        }
                        goto cleanup;
                    }

                    // Calculate RTT
                    gettimeofday(&end, NULL);
                    long long rtt = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
                    data->total_rtt += rtt;
                    data->total_messages++;
                    waiting_for_response = 0;

                    // Modify event to listen for EPOLLOUT again
                    event.events = EPOLLIN | EPOLLOUT;
                    event.data.fd = data->socket_fd;
                    epoll_ctl(data->epoll_fd, EPOLL_CTL_MOD, data->socket_fd, &event);
                }

                if (events[i].events & EPOLLOUT && !waiting_for_response && remaining_requests > 0)
                {
                    // Socket is ready for writing - send next request
                    gettimeofday(&start, NULL);
                    int bytes_sent = send(data->socket_fd, send_buf, MESSAGE_SIZE, 0);
                    if (bytes_sent <= 0)
                    {
                        perror("send");
                        goto cleanup;
                    }

                    remaining_requests--;
                    waiting_for_response = 1;

                    // Modify event to only listen for EPOLLIN
                    event.events = EPOLLIN;
                    event.data.fd = data->socket_fd;
                    epoll_ctl(data->epoll_fd, EPOLL_CTL_MOD, data->socket_fd, &event);
                }
            }
        }
    }

cleanup:
    // Calculate request rate
    if (data->total_messages > 0)
    {
        float total_seconds = data->total_rtt / 1000000.0;
        data->request_rate = data->total_messages / total_seconds;
    }
    else
    {
        data->request_rate = 0;
    }

    return NULL;
}

/*
 * This function orchestrates multiple client threads to send requests to a server,
 * collect performance data of each threads, and compute aggregated metrics of all threads.
 */
void run_client()
{
    pthread_t threads[num_client_threads];
    client_thread_data_t thread_data[num_client_threads];
    struct sockaddr_in server_addr;

    // Initialize server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr.s_addr = inet_addr(server_ip);

    // Create sockets and epoll instances for client threads
    for (int i = 0; i < num_client_threads; i++)
    {
        // Create socket
        thread_data[i].socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (thread_data[i].socket_fd == -1)
        {
            perror("socket");
            exit(EXIT_FAILURE);
        }

        // Connect to server
        if (connect(thread_data[i].socket_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
        {
            perror("connect");
            close(thread_data[i].socket_fd);
            exit(EXIT_FAILURE);
        }

        // Set socket to non-blocking mode
        int flags = fcntl(thread_data[i].socket_fd, F_GETFL, 0);
        fcntl(thread_data[i].socket_fd, F_SETFL, flags | O_NONBLOCK);

        // Create epoll instance
        thread_data[i].epoll_fd = epoll_create1(0);
        if (thread_data[i].epoll_fd == -1)
        {
            perror("epoll_create1");
            close(thread_data[i].socket_fd);
            exit(EXIT_FAILURE);
        }

        // Initialize other fields
        thread_data[i].total_rtt = 0;
        thread_data[i].total_messages = 0;
        thread_data[i].request_rate = 0;
    }

    // Create client threads
    for (int i = 0; i < num_client_threads; i++)
    {
        pthread_create(&threads[i], NULL, client_thread_func, &thread_data[i]);
    }

    // Wait for client threads to complete and aggregate metrics
    long long total_rtt = 0;
    long total_messages = 0;
    float total_request_rate = 0;

    for (int i = 0; i < num_client_threads; i++)
    {
        pthread_join(threads[i], NULL);
        total_rtt += thread_data[i].total_rtt;
        total_messages += thread_data[i].total_messages;
        total_request_rate += thread_data[i].request_rate;

        // Clean up resources
        close(thread_data[i].socket_fd);
        close(thread_data[i].epoll_fd);
    }

    if (total_messages > 0)
    {
        long long average_rtt = total_rtt / total_messages;
        float average_request_rate = total_request_rate / num_client_threads;
        //      printf("Total RTT: %lld us\n", total_rtt);
        //      printf("Total Messages: %ld\n", total_messages);
        printf("Average RTT: %lld us\n", average_rtt);
        printf("Total Request Rate: %f messages/s\n", total_request_rate);

        //        printf("Average Request Rate: %f messages/s\n", average_request_rate);
        // printf("RTTxRPS = %f\n", average_rtt * average_request_rate);

        //    printf("total_rtt = %lld, total_messages = %ld, total_request_rate = %f\n", total_rtt, total_messages, total_request_rate);
        //  printf("average_rtt = %lld, average_request_rate = %f\n", average_rtt, average_request_rate);
    }
    else
    {
        printf("No messages were sent.\n");
    }
}

void run_server()
{
    int listen_fd, epoll_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct epoll_event event, events[MAX_EVENTS];

    // Create listening socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd == -1)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Set socket options to reuse address
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        perror("setsockopt");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    // Set up server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(server_ip);
    server_addr.sin_port = htons(server_port);

    // Bind socket to address
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1)
    {
        perror("bind");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    // Listen for connections
    if (listen(listen_fd, SOMAXCONN) == -1)
    {
        perror("listen");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    // Create epoll instance
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1)
    {
        perror("epoll_create1");
        close(listen_fd);
        exit(EXIT_FAILURE);
    }

    // Add listening socket to epoll
    event.events = EPOLLIN;
    event.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event) == -1)
    {
        perror("epoll_ctl: listen_fd");
        close(listen_fd);
        close(epoll_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server started on %s:%d\n", server_ip, server_port);

    // Server's run-to-completion event loop
    while (1)
    {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1)
        {
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < nfds; i++)
        {
            if (events[i].data.fd == listen_fd)
            {
                // New connection
                int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd == -1)
                {
                    perror("accept");
                    continue;
                }

                // Set socket to non-blocking mode
                int flags = fcntl(client_fd, F_GETFL, 0);
                fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                // Add client socket to epoll
                event.events = EPOLLIN | EPOLLET; // Edge-triggered mode
                event.data.fd = client_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) == -1)
                {
                    perror("epoll_ctl: client_fd");
                    close(client_fd);
                    continue;
                }

                printf("New client connected: %s:%d\n",
                       inet_ntoa(client_addr.sin_addr),
                       ntohs(client_addr.sin_port));
            }
            else
            {
                // Existing connection - handle data
                char buffer[MESSAGE_SIZE];
                int client_fd = events[i].data.fd;

                // Read data from client
                int bytes_read = recv(client_fd, buffer, MESSAGE_SIZE, 0);
                if (bytes_read <= 0)
                {
                    if (bytes_read == 0)
                    {
                        // Connection closed
                        printf("Client disconnected\n");
                    }
                    else if (errno != EAGAIN && errno != EWOULDBLOCK)
                    {
                        perror("recv");
                    }

                    // Remove from epoll and close socket
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                    close(client_fd);
                    continue;
                }

                // Debug message to help identify issues
                printf("Received message of %d bytes from client\n", bytes_read);

                // Echo the data back to client - only echo exactly what was received
                int bytes_sent = send(client_fd, buffer, bytes_read, 0);
                if (bytes_sent <= 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                {
                    perror("send");
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                    close(client_fd);
                }
            }
        }
    }

    // Cleanup
    close(listen_fd);
    close(epoll_fd);
}

int main(int argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "server") == 0)
    {
        if (argc > 2)
            server_ip = argv[2];
        if (argc > 3)
            server_port = atoi(argv[3]);

        run_server();
    }
    else if (argc > 1 && strcmp(argv[1], "client") == 0)
    {
        if (argc > 2)
            server_ip = argv[2];
        if (argc > 3)
            server_port = atoi(argv[3]);
        if (argc > 4)
            num_client_threads = atoi(argv[4]);
        if (argc > 5)
            num_requests = atoi(argv[5]);

        run_client();
    }
    else
    {
        printf("Usage: %s <server|client> [server_ip server_port num_client_threads num_requests]\n", argv[0]);
    }

    return 0;
}
