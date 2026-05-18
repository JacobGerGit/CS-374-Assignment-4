/*
 * Author: Jacob Gervais
 * GitHub username: JacobGerGit
 * Date: 5/24/2026
 * Description: This file contains the implementation for smallsh.
 *              It supports built in commands, foreground and background
 *              processes, redirection, and signal handling.
 */

// Get rid of red squiggly lines
#define _POSIX_C_SOURCE 200809L

// prints
#include <stdio.h>
// bools
#include <stdbool.h>
// strcmp, strtok, etc.
#include <string.h>
// fork, exec, etc.
#include <unistd.h>
// EXIT_SUCCESS
#include <stdlib.h>
// data types
#include <sys/types.h>
// waitpid
#include <sys/wait.h>
// file control
#include <fcntl.h>
// signals
#include <signal.h>
// interrupted sys calls
#include <errno.h>

// Define statements
#define MAXINPUTLEN 2048
#define MAXARGS 512
#define MAXBGPROC 100

// Command line struct
struct commandLine
{
    char* argv[MAXARGS + 1];
    int argc;
    char* inputFile;
    char* outputFile;
    bool backgroundProc;
};

// A few useful global vars
int exitStatus = 0;
int signalStatus = 0;
bool wasStatusSignal = false;

pid_t backgroundPids[MAXBGPROC];
int backgroundCount = 0;

// Foreground-only mode
volatile sig_atomic_t foregroundOnlyMode = 0;
volatile sig_atomic_t foregroundProcessRunning = 0;
volatile sig_atomic_t printForegroundMessage = 0;


void handleSigtstp(int signo)
{
    // Ctrl-Z toggles foreground-only mode and decides whether to print the message now or later
    char enterMessage[] = "\nEntering foreground-only mode (& is now ignored)\n";
    char exitMessage[] = "\nExiting foreground-only mode\n";

    (void) signo;

    // Toggle foreground-only mode on or off
    if (foregroundOnlyMode == 0)
    {
        foregroundOnlyMode = 1;
        printForegroundMessage = 1;
    }
    else
    {
        foregroundOnlyMode = 0;
        printForegroundMessage = 2;
    }

    // If the shell is at the prompt, print the message right away
    if (foregroundProcessRunning == 0)
    {
        if (printForegroundMessage == 1)
        {
            write(STDOUT_FILENO, enterMessage, sizeof(enterMessage) - 1);
        }
        else
        {
            write(STDOUT_FILENO, exitMessage, sizeof(exitMessage) - 1);
        }

        printForegroundMessage = 0;
    }
}

void signalHandlers()
{
    // Sets up how the parent shell handles Ctrl-C and Ctrl-Z
    struct sigaction SIGINT_action = {0};
    struct sigaction SIGTSTP_action = {0};

    SIGINT_action.sa_handler = SIG_IGN;
    sigfillset(&SIGINT_action.sa_mask);
    SIGINT_action.sa_flags = 0;
    sigaction(SIGINT, &SIGINT_action, NULL);

    SIGTSTP_action.sa_handler = handleSigtstp;
    sigfillset(&SIGTSTP_action.sa_mask);
    SIGTSTP_action.sa_flags = 0;
    sigaction(SIGTSTP, &SIGTSTP_action, NULL);
}


void printFgMsg()
{
    // Prints the SIGTSTP message after a foreground process has ended.
    // Print whichever foreground-only message is waiting
    if (printForegroundMessage == 1)
    {
        printf("\nEntering foreground-only mode (& is now ignored)\n");
        fflush(stdout);
        printForegroundMessage = 0;
    }
    else if (printForegroundMessage == 2)
    {
        printf("\nExiting foreground-only mode\n");
        fflush(stdout);
        printForegroundMessage = 0;
    }
}


void makeCmd(struct commandLine* currCommand)
{
    // Set the command struct back to a clean state
    int index;

    currCommand->argc = 0;
    currCommand->inputFile = NULL;
    currCommand->outputFile = NULL;
    currCommand->backgroundProc = false;

    for (index = 0; index < MAXARGS + 1; index++)
    {
        currCommand->argv[index] = NULL;
    }
}


void freeCmdMem(struct commandLine* currCommand)
{
    // Frees the memory allocated by strdup
    int index;

    // Free each command argument
    for (index = 0; index < currCommand->argc; index++)
    {
        free(currCommand->argv[index]);
        currCommand->argv[index] = NULL;
    }

    // Free input and output files if they were used
    if (currCommand->inputFile != NULL)
    {
        free(currCommand->inputFile);
        currCommand->inputFile = NULL;
    }

    if (currCommand->outputFile != NULL)
    {
        free(currCommand->outputFile);
        currCommand->outputFile = NULL;
    }
}


void inputParser(char* input, struct commandLine* currCommand)
{
    // Tokenize the user’s input and store those pieces in the command struct
    char* token;
    char* tokens[MAXARGS + 5];
    int tokenCount = 0;
    int index = 0;

    token = strtok(input, " \n");

    // Store each word from the input line
    while (token != NULL && tokenCount < MAXARGS + 5)
    {
        tokens[tokenCount] = token;
        tokenCount++;
        token = strtok(NULL, " \n");
    }

    // Early exit if there is nothing to parse
    if (tokenCount == 0)
    {
        return;
    }

    // Check if last word is &, if so make it a background command
    if (strcmp(tokens[tokenCount - 1], "&") == 0)
    {
        currCommand->backgroundProc = true;
        tokenCount--;
    }

    // Sort tokens into args, input file, or output file
    while (index < tokenCount)
    {
        if (strcmp(tokens[index], "<") == 0)
        {
            currCommand->inputFile = strdup(tokens[index + 1]);
            index += 2;
        }
        else if (strcmp(tokens[index], ">") == 0)
        {
            currCommand->outputFile = strdup(tokens[index + 1]);
            index += 2;
        }
        else
        {
            currCommand->argv[currCommand->argc] = strdup(tokens[index]);
            currCommand->argc++;
            index++;
        }
    }

    currCommand->argv[currCommand->argc] = NULL;
}


bool isEmpty(char* input)
{
    // Check for blank or # to know whether to ignore line
    int index = 0;

    // Ignore lines that start with a comment
    if (input[0] == '#')
    {
        return true;
    }

    // Look for any real character that is not just whitespace
    while (input[index] != '\0')
    {
        if (input[index] == '\n')
        {
            return true;
        }

        if (input[index] != ' ' && input[index] != '\t')
        {
            return false;
        }

        index++;
    }

    return true;
}


bool isBuiltIn(struct commandLine* currCommand)
{
    // Check whether the command is exit, cd, or status
    if (strcmp(currCommand->argv[0], "exit") == 0)
    {
        return true;
    }
    else if (strcmp(currCommand->argv[0], "cd") == 0)
    {
        return true;
    }
    else if (strcmp(currCommand->argv[0], "status") == 0)
    {
        return true;
    }

    return false;
}

// Print the status of the last foreground process
void printStatus()
{
    if (wasStatusSignal)
    {
        printf("terminated by signal %d\n", signalStatus);
    }
    else
    {
        printf("exit value %d\n", exitStatus);
    }

    fflush(stdout);
}

// Add a background process to the array, so long as there is room
void addBgProc(pid_t processId)
{
    if (backgroundCount < MAXBGPROC)
    {
        backgroundPids[backgroundCount] = processId;
        backgroundCount++;
    }
}

// Removes a background process from the array, then fixes the array
void rmBgProc(int index)
{
    int currIndex;

    for (currIndex = index; currIndex < backgroundCount - 1; currIndex++)
    {
        backgroundPids[currIndex] = backgroundPids[currIndex + 1];
    }

    backgroundCount--;
}

// Loop through bg proc array and check if any of them have ended
void checkBgProc()
{
    int index = 0;
    int childStatus;
    pid_t finishedPid;

    while (index < backgroundCount)
    {
        finishedPid = waitpid(backgroundPids[index], &childStatus, WNOHANG);

        // This background process is still running
        if (finishedPid == 0)
        {
            index++;
        }

        // Something went wrong, so remove it from the list
        else if (finishedPid == -1)
        {
            rmBgProc(index);
        }

        else
        {
            // This background process finished
            if (WIFEXITED(childStatus))
            {
                printf("background pid %d is done: exit value %d\n",
                       finishedPid, WEXITSTATUS(childStatus));
                fflush(stdout);
            }
            else if (WIFSIGNALED(childStatus))
            {
                printf("background pid %d is done: terminated by signal %d\n",
                       finishedPid, WTERMSIG(childStatus));
                fflush(stdout);
            }

            rmBgProc(index);
        }
    }
}

// If user types exit, terminate all bg procs
void killBgProc()
{
    int index;

    for (index = 0; index < backgroundCount; index++)
    {
        kill(backgroundPids[index], SIGTERM);
    }

    for (index = 0; index < backgroundCount; index++)
    {
        waitpid(backgroundPids[index], NULL, 0);
    }
}

// Handles exit, cd, and status. No child process required
void runCmd(struct commandLine* currCommand)
{
    char * homePath;

    // Choose which built in command to run
    if (strcmp(currCommand->argv[0], "exit") == 0)
    {
        killBgProc();
        freeCmdMem(currCommand);
        exit(EXIT_SUCCESS);
    }

    else if (strcmp(currCommand->argv[0], "cd") == 0)
    {
        // If cd has no argument, go to HOME
        if (currCommand->argc == 1)
        {
            homePath = getenv("HOME");
            chdir(homePath);
        }
        else
        {
            if (chdir(currCommand->argv[1]) == -1)
            {
                perror("cd");
                fflush(stderr);
            }
        }
    }

    else if (strcmp(currCommand->argv[0], "status") == 0)
    {
        printStatus();
    }
}

// Runs all commands that are not built in
void runExtCmd(struct commandLine * currCommand)
{
    pid_t spawnPid;
    int childStatus;
    int sourceFD;
    int targetFD;
    int result;
    struct sigaction child_SIGINT_action = {0};
    struct sigaction child_SIGTSTP_action = {0};

    // If foreground-only mode is active, ignore the background option
    if (foregroundOnlyMode == 1)
    {
        currCommand->backgroundProc = false;
    }

    // Fork a child process for the external command
    spawnPid = fork();

    // Handle fork result
    if (spawnPid == -1)
    {
        perror("fork");
        fflush(stderr);
        exitStatus = 1;
        wasStatusSignal = false;
    }

    else if (spawnPid == 0)
    {
        // Set up Ctrl-C behavior for foreground and background child processes
        if (currCommand->backgroundProc)
        {
            child_SIGINT_action.sa_handler = SIG_IGN;
        }
        else
        {
            child_SIGINT_action.sa_handler = SIG_DFL;
        }

        sigfillset(&child_SIGINT_action.sa_mask);
        child_SIGINT_action.sa_flags = 0;
        sigaction(SIGINT, &child_SIGINT_action, NULL);

        // Make children ignore Ctrl-Z because only the parent shell handles foreground-only mode
        child_SIGTSTP_action.sa_handler = SIG_IGN;
        sigfillset(&child_SIGTSTP_action.sa_mask);
        child_SIGTSTP_action.sa_flags = 0;
        sigaction(SIGTSTP, &child_SIGTSTP_action, NULL);

        // Handle input redirection, or use /dev/null for background commands with no input file
        // Redirect input if the user gave an input file
        if (currCommand->inputFile != NULL)
        {
            sourceFD = open(currCommand->inputFile, O_RDONLY);

            // Exit child if input file cannot be opened
            if (sourceFD == -1)
            {
                printf("cannot open %s for input\n", currCommand->inputFile);
                fflush(stdout);
                exit(1);
            }

            result = dup2(sourceFD, 0);

            if (result == -1)
            {
                perror("dup2");
                fflush(stderr);
                exit(1);
            }

            close(sourceFD);
        }
        else if (currCommand->backgroundProc)
        {
            sourceFD = open("/dev/null", O_RDONLY);

            if (sourceFD == -1)
            {
                perror("/dev/null");
                fflush(stderr);
                exit(1);
            }

            result = dup2(sourceFD, 0);

            if (result == -1)
            {
                perror("dup2");
                fflush(stderr);
                exit(1);
            }

            close(sourceFD);
        }

        // Handle output redirection, or use /dev/null for background commands with no output file
        // Redirect output if the user gave an output file
        if (currCommand->outputFile != NULL)
        {
            targetFD = open(currCommand->outputFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);

            // Exit child if output file cannot be opened
            if (targetFD == -1)
            {
                printf("cannot open %s for output\n", currCommand->outputFile);
                fflush(stdout);
                exit(1);
            }

            result = dup2(targetFD, 1);

            if (result == -1)
            {
                perror("dup2");
                fflush(stderr);
                exit(1);
            }

            close(targetFD);
        }
        else if (currCommand->backgroundProc)
        {
            targetFD = open("/dev/null", O_WRONLY);

            if (targetFD == -1)
            {
                perror("/dev/null");
                fflush(stderr);
                exit(1);
            }

            result = dup2(targetFD, 1);

            if (result == -1)
            {
                perror("dup2");
                fflush(stderr);
                exit(1);
            }

            close(targetFD);
        }

        // Replace the child process with the command, or print an error if execvp fails
        execvp(currCommand->argv[0], currCommand->argv);

        perror(currCommand->argv[0]);
        fflush(stderr);
        exit(1);
    }

    else
    {
        // Parent either tracks a background process or waits for a foreground process
        // Background commands return control to the user right away
        if (currCommand->backgroundProc)
        {
            printf("background pid is %d\n", spawnPid);
            fflush(stdout);
            addBgProc(spawnPid);
        }
        else
        {
            // Foreground commands make the parent wait
            foregroundProcessRunning = 1;

            // If waitpid gets interrupted by Ctrl-Z, keep waiting for the foreground process
            while (waitpid(spawnPid, &childStatus, 0) == -1)
            {
                if (errno != EINTR)
                {
                    perror("waitpid");
                    fflush(stderr);
                    break;
                }
            }

            foregroundProcessRunning = 0;

            // Save how the foreground process ended
            if (WIFEXITED(childStatus))
            {
                exitStatus = WEXITSTATUS(childStatus);
                signalStatus = 0;
                wasStatusSignal = false;
            }
            else if (WIFSIGNALED(childStatus))
            {
                signalStatus = WTERMSIG(childStatus);
                wasStatusSignal = true;

                printf("terminated by signal %d\n", signalStatus);
                fflush(stdout);
            }

            printFgMsg();
        }
    }
}


int main()
{
    char input[MAXINPUTLEN];
    struct commandLine currCommand;

    signalHandlers();

    while(true)
    {
        // Check background processes before showing the next prompt
        checkBgProc();
        printFgMsg();

        printf(": ");
        fflush(stdout);

        // If fgets is interrupted by a signal, reset stdin and keep the shell running
        if (fgets(input, MAXINPUTLEN, stdin) == NULL)
        {
            clearerr(stdin);
            continue;
        }

        // Ignore blank lines and comments
        if (isEmpty(input))
        {
            continue;
        }

        makeCmd(&currCommand);
        inputParser(input, &currCommand);

        // If parsing found no command, start over
        if (currCommand.argc == 0)
        {
            freeCmdMem(&currCommand);
            continue;
        }

        // Run built in commands directly, otherwise fork
        if (isBuiltIn(&currCommand))
        {
            runCmd(&currCommand);
        }
        else
        {
            runExtCmd(&currCommand);
        }

        freeCmdMem(&currCommand);
    }

    return EXIT_SUCCESS;
}