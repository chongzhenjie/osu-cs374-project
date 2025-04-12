#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define INPUT_LENGTH 2048
#define MAX_ARGS 512

int is_fg_only = 0; // flag that indicates when program is in foreground-only mode
int last_status = 0; // records terminating signal of last foreground process

// structure for parsing command line input
struct command_line {
	char *argv[MAX_ARGS + 1];
	int argc;
	char *input_file;
	char *output_file;
	bool is_bg;
};

// Function to parse user inputs from command line
// Return: Structure storing the various inputs
struct command_line *parse_input(void) {
	char input[INPUT_LENGTH];
	struct command_line *curr_command = (struct command_line *) calloc(1, sizeof(struct command_line));

	// get input
	printf(": ");
	fflush(stdout);
	fgets(input, INPUT_LENGTH, stdin);

	// tokenize the input
	char *token = strtok(input, " \n");
	while (token) {
		if (strcmp(token, "<") == 0) {
			curr_command->input_file = strdup(strtok(NULL, " \n"));
		} 
        else if (strcmp(token, ">") == 0) {
			curr_command->output_file = strdup(strtok(NULL, " \n"));
		} 
        else if (strcmp(token, "&") == 0) {
			curr_command->is_bg = true;
		} 
        else {
			curr_command->argv[curr_command->argc++] = strdup(token);
		}
		token = strtok(NULL," \n");
	}
    return curr_command;
}

// Function to handle SIGTSTP that toggles foreground-only mode
// Arguments: Signal number received
void handle_SIGTSTP(int signo) {
    // switch modes and display messages
    if (is_fg_only == 0) {
        char* message = "\nEntering foreground-only mode (& is now ignored)\n: ";
        is_fg_only = 1;
        write(STDOUT_FILENO, message, 52);
    }
    else {
        char* message = "\nExiting foreground-only mode\n: ";
        is_fg_only = 0;
        write(STDOUT_FILENO, message, 32);
    }
}

int main()
{
	struct command_line *curr_command;
    int child_status;
    pid_t spawn_pid;

    // set up SIGTSTP handler in shell
    struct sigaction SIGTSTP_action = {0};
    SIGTSTP_action.sa_handler = handle_SIGTSTP;
    sigfillset(&SIGTSTP_action.sa_mask);
    SIGTSTP_action.sa_flags = SA_RESTART;
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);

    // shell to ignore SIGINT
    struct sigaction ignore_action = {0};
    ignore_action.sa_handler = SIG_IGN;
    sigfillset(&ignore_action.sa_mask);
    ignore_action.sa_flags = 0;
    sigaction(SIGINT, &ignore_action, NULL);

	while(true)
	{
        // check for completed background processes
        while ((spawn_pid = waitpid(-1, &child_status, WNOHANG)) > 0) {
            if (WIFEXITED(child_status)) {
                printf("background pid %d is done: exit value %d\n", spawn_pid, WEXITSTATUS(child_status));
            }
            else if (WIFSIGNALED(child_status)) {
                printf("background pid %d is done: terminated by signal %d\n", spawn_pid, WTERMSIG(child_status));
            }
            fflush(stdout);
        }

        // parse command line input
		curr_command = parse_input();

        // ignore comment lines and empty lines
        if (curr_command->argc == 0 || strncmp(curr_command->argv[0], "#", 1) == 0) {
            continue;
        }

        // built-in command for exit
        else if (strcmp(curr_command->argv[0], "exit") == 0) {
            kill(0, SIGTERM);
            exit(0);
        }

        // built-in command for cd
        else if (strcmp(curr_command->argv[0], "cd") == 0) {
            if (curr_command->argc > 1) {
                if (chdir(curr_command->argv[1])) {
                    perror("cd");
                }
            }
            else {
                chdir(getenv("HOME"));
            }
            continue;
        }

        // built-in command for status
        else if (strcmp(curr_command->argv[0], "status") == 0) {
            if (WIFSIGNALED(last_status)) {
                printf("terminated by signal %d\n", WTERMSIG(last_status));
            }
            else {
                printf("exit value %d\n", WEXITSTATUS(last_status));
            }
            continue;
        }

        // ignore is_bg flag if in foreground-only mode
        if (is_fg_only == 1) {
            curr_command->is_bg = false;
        }

        // create child process to execute other commands
        spawn_pid = fork();
        switch (spawn_pid) {
            case -1: {
                perror("fork");
                break;
            }

            case 0: {

                // child to ignore SIGTSTP
                struct sigaction child_ignore_action = {0};
                child_ignore_action.sa_handler = SIG_IGN;
                sigfillset(&child_ignore_action.sa_mask);
                child_ignore_action.sa_flags = 0;
                sigaction(SIGTSTP, &child_ignore_action, NULL);

                // child to restore default if foreground process
                struct sigaction child_SIGINT_action = {0};
                if (curr_command->is_bg) {
                    child_SIGINT_action.sa_handler = SIG_IGN;
                }
                else {
                    child_SIGINT_action.sa_handler = SIG_DFL;
                }
                sigfillset(&child_SIGINT_action.sa_mask);
                child_SIGINT_action.sa_flags = 0;
                sigaction(SIGINT, &child_SIGINT_action, NULL);

                // input redirection
                if (curr_command->input_file != NULL) {
                    int source_fd = open(curr_command->input_file, O_RDONLY);
                    if (source_fd == -1) {
                        fprintf(stderr, "cannot open %s for input\n", curr_command->input_file);
                        exit(EXIT_FAILURE);
                    }
                    if (dup2(source_fd, 0) == -1) {
                        perror("dup2");
                        exit(EXIT_FAILURE);
                    }
                    close(source_fd);
                }
                // for background process without specified input
                else if (curr_command->is_bg) {
                    int source_fd = open("/dev/null", O_RDONLY);
                    if (source_fd == -1) {
                        perror("open");
                        exit(EXIT_FAILURE);
                    }
                    if (dup2(source_fd, 0) == -1) {
                        perror("dup2");
                        exit(1);
                    }
                    close(source_fd);
                }

                // output redirection
                if (curr_command->output_file != NULL) {
                    int target_fd = open(curr_command->output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                    if (target_fd == -1) {
                        perror("open");
                        exit(EXIT_FAILURE);
                    }
                    if(dup2(target_fd, 1) == -1) {
                        perror("dup2");
                        exit(EXIT_FAILURE);
                    }
                    close(target_fd);
                }
                // for background process without specified output
                else if (curr_command->is_bg) {
                    int target_fd = open("/dev/null", O_WRONLY);
                    if (target_fd == -1) {
                        perror("open");
                        exit(EXIT_FAILURE);
                    }
                    if (dup2(target_fd, 1) == -1) {
                        perror("dup2");
                        exit(EXIT_FAILURE);
                    }
                    close(target_fd);
                }

                // execute command
                if (execvp(curr_command->argv[0], curr_command->argv) == -1) {
                    perror(curr_command->argv[0]);
                    exit(EXIT_FAILURE);
                };
                break;
            }
            default: {
                if (curr_command->is_bg) {
                    printf("background pid is %d\n", spawn_pid);
                    fflush(stdout);
                }
                else {
                    
                    // parent wait for child in foreground process
                    spawn_pid = waitpid(spawn_pid, &child_status, 0);
                    if (WIFSIGNALED(child_status)) {
                        printf("terminated by signal %d\n", WTERMSIG(child_status));
                        fflush(stdout);
                    }
                    last_status = child_status;
                }
            }
        }
	}
	return EXIT_SUCCESS;
}