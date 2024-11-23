#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <asm-generic/socket.h>
#include <sys/wait.h>

/* Define buffer size for network communication */
#define BUFFER_SIZE 4096

void SendError(int Client_Fd, int code, const char *message)
{
    char response[BUFFER_SIZE];

    /* Format the HTTP error response */
    snprintf(response, sizeof(response),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: text/html\r\n\r\n"
             "<html><body><h1>%d %s</h1></body></html>",
             code, message, code, message);

    /* Send the error response */
    ssize_t bytes_sent = write(Client_Fd, response, strlen(response));
    if (bytes_sent < 0)
    {
        /* Handle write failure */
        perror("Write system call failed");
    }
}

int ServerSetup(int port)
{
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    /* Create the server socket */
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    /* Set socket options to reuse address and port */
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
    {
        perror("Setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    /* Set up the server address */
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    /* Bind the socket to the address */
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    /* Listen for incoming connections */
    if (listen(server_fd, 10) < 0)
    {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Server started on port %d\n", port); /* Print server startup message */
    return server_fd;
}

void ListDirectory(int Client_Fd, const char *path)
{
    DIR *dir = opendir(path); /* Open the directory */
    if (!dir)
    {
        SendError(Client_Fd, 500, "Internal Server Error");
        return;
    }

    char response[BUFFER_SIZE];
    /* Send the HTTP headers for directory listing */
    snprintf(response, sizeof(response),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/html\r\n\r\n"
             "<html><body><h1>Directory Listing</h1><ul>");
    ssize_t bytes_sent = write(Client_Fd, response, strlen(response));
    if (bytes_sent < 0)
    {
        perror("Write system call failed");
    }

    struct dirent *entry;
    /* Read and list the directory contents */
    while ((entry = readdir(dir)) != NULL)
    {
        snprintf(response, sizeof(response), "<li><a href=\"%s\">%s</a></li>", entry->d_name, entry->d_name);
        bytes_sent = write(Client_Fd, response, strlen(response));
        if (bytes_sent < 0)
        {
            perror("Write system call failed");
        }
    }

    snprintf(response, sizeof(response), "</ul></body></html>");
    bytes_sent = write(Client_Fd, response, strlen(response));
    if (bytes_sent < 0)
    {
        perror("Write system call failed");
    }
    closedir(dir); /* Close the directory */
}

void SendFile(int Client_Fd, const char *path)
{
    struct stat statbuf;

    /* Check if the file exists and get its details */
    if (stat(path, &statbuf) < 0)
    {
        SendError(Client_Fd, 404, "Requested file is Not Found");
        return;
    }

    /* If it's a CGI file, execute it using execv */
    if (strstr(path, ".cgi") != NULL)
    {
        int pipefd[2];
        /* Create a pipe to capture CGI script output */
        if (pipe(pipefd) == -1)
        {
            SendError(Client_Fd, 500, "Internal Server Error");
            return;
        }

        pid_t pid = fork();
        if (pid == -1)
        {
            SendError(Client_Fd, 500, "Internal Server Error");
            return;
        }

        if (pid == 0)
        {                                        /* Child process: execute the CGI script */
            close(pipefd[0]);                    /* Close the read end of the pipe */
            dup2(pipefd[1], STDOUT_FILENO);      /* Redirect stdout to the pipe */
            char *argv[] = {(char *)path, NULL}; /* Prepare arguments for execv */
            execv(path, argv);                   /* Execute the CGI script */

            perror("execv failed");
            exit(1);
        }
        else
        {                     /* Parent process: read from pipe and send to client */
            close(pipefd[1]); /* Close the write end of the pipe */

            /* Send HTTP headers to the client */
            char response[BUFFER_SIZE];
            snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n");
            ssize_t bytes_sent = write(Client_Fd, response, strlen(response));
            if (bytes_sent < 0)
            {
                perror("Write system call failed");
            }

            /* Read from the pipe and send the CGI output to the client */
            char buffer[BUFFER_SIZE];
            ssize_t bytes_read;
            while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer))) > 0)
            {
                bytes_sent = write(Client_Fd, buffer, bytes_read);
                if (bytes_sent < 0)
                {
                    perror("Write system call failed");
                }
            }

            close(pipefd[0]);
        }
    }
    /* If it's a regular file, serve it */
    else
    {
        int file_fd = open(path, O_RDONLY);
        if (file_fd < 0)
        {
            SendError(Client_Fd, 500, "Internal Server Error");
            return;
        }

        /* Send HTTP headers for the regular file */
        char response[BUFFER_SIZE];
        snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n");
        ssize_t bytes_sent = write(Client_Fd, response, strlen(response));
        if (bytes_sent < 0)
        {
            perror("Write system call failed");
        }

        /* Read and send the file content */
        ssize_t bytes;
        while ((bytes = read(file_fd, response, sizeof(response))) > 0)
        {
            bytes_sent = write(Client_Fd, response, bytes);
            if (bytes_sent < 0)
            {
                perror("Write system call failed");
            }
        }

        close(file_fd); /* Close the file after sending */
    }
}

void HandleClient(int Client_Fd)
{
    char buffer[BUFFER_SIZE] = {0};
    int bytes_read = read(Client_Fd, buffer, sizeof(buffer) - 1);

    if (bytes_read <= 0)
    {
        perror("Failed to read from client");
        close(Client_Fd);
        return;
    }

    printf("Request:\n%s\n", buffer); /* Print the HTTP request */
    printf("================================================================================\n");

    char method[16], path[256], protocol[16];
    sscanf(buffer, "%s %s %s", method, path, protocol);

    /* Remove the leading '/' from the path if present */
    if (path[0] == '/')
        memmove(path, path + 1, strlen(path));
    if (strlen(path) == 0)
        strcpy(path, "."); /* Default to current directory if no path is provided */

    struct stat file_stat;
    /* Check if the file/directory exists */
    if (stat(path, &file_stat) == -1)
    {
        SendError(Client_Fd, 404, "Requested file is Not Found");
    }
    else if (S_ISDIR(file_stat.st_mode))
    {
        ListDirectory(Client_Fd, path); /* List directory contents */
    }
    else if (S_ISREG(file_stat.st_mode))
    {
        SendFile(Client_Fd, path); /* Send the requested file */
    }
    else
    {
        SendError(Client_Fd, 403, "Forbidden"); /* Handle forbidden request */
    }

    close(Client_Fd); /* Close the client connection */
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535)
    {
        fprintf(stderr, "Invalid port number: %s\n", argv[1]);
        exit(EXIT_FAILURE);
    }

    int server_fd = ServerSetup(port); /* Set up the server */

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    for (;;)
    {
        /* Accept incoming client connections */
        int Client_Fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (Client_Fd < 0)
        {
            perror("Accept failed");
            continue;
        }

        printf("Connected: %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        pid_t pid = fork(); /* Fork a child process to handle the client */
        if (pid < 0)
        {
            perror("Fork failed");
            close(Client_Fd);
            continue;
        }

        /* Child Process */
        if (pid == 0)
        {
            close(server_fd);        /* Close the server socket in the child process */
            HandleClient(Client_Fd); /* Handle the client request */
            close(Client_Fd);
            exit(0);
        }

        /* Parent Process */
        else
        {
            close(Client_Fd); /* Close the client socket in the parent process */

            int status;
            while (waitpid(-1, &status, WNOHANG) > 0)
                ; /* Reap the child processes */
        }
    }

    close(server_fd); /* Close the server socket when done */
    return 0;
}
