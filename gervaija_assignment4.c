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

// 
#define MAXINPUTLEN 2048
#define MAXARGS 512
#define MAXBGPROC 100


struct commandLine
{
    char* argv[MAXARGS + 1];
    int argc;
    char* inputFile;
    char* outputFile;
    bool backgroundProc;
};


/*
 * These are global because they need to be accessed from different functions.
 * I usually avoid globals, but they make sense here because the shell needs to
 * remember status and signal mode for the whole time it is running.
 */
int lastExitStatus = 0;
int lastSignalStatus = 0;
bool lastStatusWasSignal = false;

pid_t backgroundPids[MAXBGPROC];
int backgroundCount = 0;

/*
 * These are used for foreground-only mode.
 * sig_atomic_t is used because these can be changed inside a signal handler.
 */
volatile sig_atomic_t foregroundOnlyMode = 0;
volatile sig_atomic_t foregroundProcessRunning = 0;
volatile sig_atomic_t printForegroundMessage = 0;


/* Function declarations */
void signalHandlers();
void handleSigtstp(int signo);

void makeCmd(struct commandLine* currCommand);
void free_command(struct commandLine* currCommand);
void inputParser(char* input, struct commandLine* currCommand);

bool isEmpty(char* input);
bool isBuiltIn(struct commandLine* currCommand);

void runCmd(struct commandLine* currCommand);
void runExtCmd(struct commandLine* currCommand);

void checkBgProc();
void addBgProc(pid_t processId);
void rmBgProc(int index);
void killBgProc();

void print_status();
void printFgMsg();


int main()
{
    char input[MAXINPUTLEN];
    struct commandLine currCommand;

    signalHandlers();

    while(true)
    {
        /*
         * Background processes are checked before each prompt.
         * This is so the message prints right before the user gets command
         * line access again.
         */
        checkBgProc();
        printFgMsg();

        printf(": ");
        fflush(stdout);

        /*
         * If fgets gets interrupted by a signal, it can return NULL.
         * In that case, clearerr lets stdin keep working.
         */
        if (fgets(input, MAXINPUTLEN, stdin) == NULL)
        {
            clearerr(stdin);
            continue;
        }

        if (isEmpty(input))
        {
            continue;
        }

        makeCmd(&currCommand);
        inputParser(input, &currCommand);

        if (currCommand.argc == 0)
        {
            free_command(&currCommand);
            continue;
        }

        if (isBuiltIn(&currCommand))
        {
            runCmd(&currCommand);
        }
        else
        {
            runExtCmd(&currCommand);
        }

        free_command(&currCommand);
    }

    return EXIT_SUCCESS;
}


void signalHandlers()
{
    /*
     * Sets up SIGINT and SIGTSTP for the parent shell.
     *
     * The parent ignores SIGINT, because CTRL-C should not kill smallsh.
     * The parent handles SIGTSTP, because CTRL-Z toggles foreground-only mode.
     */
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


void handleSigtstp(int signo)
{
    /*
     * Handles CTRL-Z.
     *
     * If no foreground process is running, print the message right away.
     * If a foreground process is running, wait until it finishes and then
     * print the message.
     */
    char enterMessage[] = "\nEntering foreground-only mode (& is now ignored)\n";
    char exitMessage[] = "\nExiting foreground-only mode\n";

    (void) signo;

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

    /*
     * printf should not be used in a signal handler.
     * write is used here because it is safe for signal handlers.
     */
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


void printFgMsg()
{
    /*
     * Prints the SIGTSTP message after a foreground process has ended.
     */
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
    /*
     * Initializes a commandLine struct before parsing into it.
     */
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


void free_command(struct commandLine* currCommand)
{
    /*
     * Frees the memory allocated by strdup.
     */
    int index;

    for (index = 0; index < currCommand->argc; index++)
    {
        free(currCommand->argv[index]);
        currCommand->argv[index] = NULL;
    }

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
    /*
     * Parses the user's command.
     *
     * I am storing the tokens first because & only means background if it is
     * the last word. If & appears anywhere else, it should just be treated as
     * a normal argument.
     */
    char* token;
    char* tokens[MAXARGS + 5];
    int tokenCount = 0;
    int index = 0;

    token = strtok(input, " \n");

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

    // Only the final token can make the command run in the background
    if (strcmp(tokens[tokenCount - 1], "&") == 0)
    {
        currCommand->backgroundProc = true;
        tokenCount--;
    }

    /*
     * Go through the tokens and separate the actual command arguments from
     * input and output redirection.
     */
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
    /*
     * Checks whether the input is blank or starts with #.
     */
    int index = 0;

    if (input[0] == '#')
    {
        return true;
    }

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
    /*
     * Checks whether the command should be handled directly by smallsh.
     */
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


void runCmd(struct commandLine* currCommand)
{
    /*
     * Runs the built in commands.
     *
     * Built in commands do not change the status value.
     * Also, if the user puts & after one of these, it is just ignored.
     */
    char* homePath;

    if (strcmp(currCommand->argv[0], "exit") == 0)
    {
        killBgProc();
        free_command(currCommand);
        exit(EXIT_SUCCESS);
    }

    else if (strcmp(currCommand->argv[0], "cd") == 0)
    {
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
        print_status();
    }
}


void print_status()
{
    /*
     * Prints the status of the last foreground process.
     */
    if (lastStatusWasSignal)
    {
        printf("terminated by signal %d\n", lastSignalStatus);
    }
    else
    {
        printf("exit value %d\n", lastExitStatus);
    }

    fflush(stdout);
}


void runExtCmd(struct commandLine* currCommand)
{
    /*
     * Runs all commands that are not built into smallsh.
     */
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

    spawnPid = fork();

    if (spawnPid == -1)
    {
        perror("fork");
        fflush(stderr);
        lastExitStatus = 1;
        lastStatusWasSignal = false;
    }

    else if (spawnPid == 0)
    {
        /*
         * This is the child process.
         *
         * Foreground children should die from CTRL-C.
         * Background children should ignore CTRL-C.
         */
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

        /*
         * All children ignore CTRL-Z.
         * Only the parent shell actually toggles foreground-only mode.
         */
        child_SIGTSTP_action.sa_handler = SIG_IGN;
        sigfillset(&child_SIGTSTP_action.sa_mask);
        child_SIGTSTP_action.sa_flags = 0;
        sigaction(SIGTSTP, &child_SIGTSTP_action, NULL);

        /*
         * Input redirection.
         * If this is a background process with no input redirection, use
         * /dev/null so it does not try to read from the terminal.
         */
        if (currCommand->inputFile != NULL)
        {
            sourceFD = open(currCommand->inputFile, O_RDONLY);

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

        /*
         * Output redirection.
         * If this is a background process with no output redirection, use
         * /dev/null so it does not print to the terminal.
         */
        if (currCommand->outputFile != NULL)
        {
            targetFD = open(currCommand->outputFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);

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

        /*
         * Replace the child process with the command.
         * If execvp returns, it means something went wrong.
         */
        execvp(currCommand->argv[0], currCommand->argv);

        perror(currCommand->argv[0]);
        fflush(stderr);
        exit(1);
    }

    else
    {
        /*
         * This is the parent process.
         *
         * If the command is in the background, do not wait.
         * If the command is in the foreground, wait until it finishes.
         */
        if (currCommand->backgroundProc)
        {
            printf("background pid is %d\n", spawnPid);
            fflush(stdout);
            addBgProc(spawnPid);
        }
        else
        {
            foregroundProcessRunning = 1;

            /*
             * waitpid can be interrupted by SIGTSTP.
             * If that happens, errno will be EINTR, so keep waiting.
             */
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

            if (WIFEXITED(childStatus))
            {
                lastExitStatus = WEXITSTATUS(childStatus);
                lastSignalStatus = 0;
                lastStatusWasSignal = false;
            }
            else if (WIFSIGNALED(childStatus))
            {
                lastSignalStatus = WTERMSIG(childStatus);
                lastStatusWasSignal = true;

                printf("terminated by signal %d\n", lastSignalStatus);
                fflush(stdout);
            }

            printFgMsg();
        }
    }
}


void addBgProc(pid_t processId)
{
    /*
     * Adds a background process to the array.
     */
    if (backgroundCount < MAXBGPROC)
    {
        backgroundPids[backgroundCount] = processId;
        backgroundCount++;
    }
}


void rmBgProc(int index)
{
    /*
     * Removes a background process from the array.
     */
    int currIndex;

    for (currIndex = index; currIndex < backgroundCount - 1; currIndex++)
    {
        backgroundPids[currIndex] = backgroundPids[currIndex + 1];
    }

    backgroundCount--;
}


void checkBgProc()
{
    /*
     * Checks if background processes have finished.
     *
     * WNOHANG means waitpid will check the process, but it will not block
     * smallsh if the process is still running.
     */
    int index = 0;
    int childStatus;
    pid_t finishedPid;

    while (index < backgroundCount)
    {
        finishedPid = waitpid(backgroundPids[index], &childStatus, WNOHANG);

        if (finishedPid == 0)
        {
            // This background process is still running
            index++;
        }

        else if (finishedPid == -1)
        {
            // Something went wrong, so remove it from the list
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


void killBgProc()
{
    /*
     * Kills all background processes before smallsh exits.
     */
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

