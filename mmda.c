/* 
 * mmda, a minimal mail delivery agent
 * Copyright: 2008 Teemu Ikonen <tpikonen@gmail.com>
 * License: GPLv2+
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pwd.h>
#include <lockfile.h>
#include <time.h>

#define SLEN 1024
#define DENY_ROOT 1
#define MAILDIR "/var/mail"
#define MAIL_GID (8)

#define SCRIPTDIR "/home/tpikonen/mailscript/git-mailscript/msmtp"

/* Check if a given shell is in /etc/shells */
void checkshell(const char *shell)
{
    char *p;

    while((p = getusershell()) != NULL) {
        if(!strcmp(p, shell)) {
            endusershell();
            return;
        }
    }
    exit(1);
}


void execprog(char * const argv[], const char *homedir)
{
    char *newenv[3];
    char *path = "PATH=/bin:/usr/bin";
    char home[SLEN];
    int i;

    snprintf(home, SLEN, "HOME=%s", homedir);
    newenv[0] = path;
    newenv[1] = home;
    newenv[2] = NULL;

    i = 0;
/*
    while(argv[i] != NULL) {
        printf("argv%d: %s\n", i, argv[i]);
        i++;
    }
*/
    execve(argv[0], argv, newenv);
//    printf("error: returning from exec\n");
    exit(1);
}


int runprog(char * const argv[], FILE *input, const char *homedir)
{
    int status;
    pid_t cpid;

    cpid = fork();
    if(cpid < 0) {
        exit(123);
    }
    if(cpid == 0) {
        dup2(fileno(input), 0);
        execprog(argv, homedir);
    }
    wait(&status);

    return status;
}

/* Make sure a correct mbox file for user exists */
/* This function is run as suid root */
void touchmbox(const char *uname, int uid)
{
    char mboxname[SLEN];
    struct stat ss;

    snprintf(mboxname, SLEN-1, "%s/%s", MAILDIR, uname);
    if(stat(mboxname, &ss) < 0) {
        int fd;
        /* touch file */
        if((fd = creat(mboxname, 0660)) < 0) {
            exit(1);
        }
        fchown(fd, uid, MAIL_GID);
        close(fd);
        stat(mboxname, &ss);
    }
    /* Check that the file is owned by the user and has correct perms */
    if(ss.st_uid != uid || ss.st_gid != MAIL_GID || !S_ISREG(ss.st_mode)
            || (ss.st_mode & 0777) != 0660 )
    {
        exit(1);
    }
}


int mboxmail(FILE* infile, const char *mboxname, const char *cname)
{
    FILE *f;
    char lockname[SLEN];
    int locksz;
    int c;
    time_t t;

    locksz = snprintf(lockname, SLEN-1, "%s.lock", mboxname);
    if((locksz != strlen(mboxname)+5) || lockfile_create(lockname, 4, 0) != 0) {
        return 13;
    }
    f = fopen(mboxname, "a");
    if(f == NULL) {
        /* Could not create mbox? */
        lockfile_remove(lockname); 
        return 3;
    }
    time(&t);
    fprintf(f, "From %s %s\n", cname, ctime(&t)); //FIXME: doesn't print timezone
    c = getc(infile);
    while(!feof(infile)) {
        putc(c, f);
        c = getc(infile);
    }
    if(lockfile_remove(lockname) != 0) {
        return 1;
    }
    if(ferror(f)) {
        /* some error */
        return 1;
    }
    fclose(f);
    return 0;
}


void eat_wspace(char *buf)
{
    int i;

    i = 0;
    while(buf[i] == ' ' || buf[i] == '\t')
        i++;
    if(i > 0) {
        strncpy(buf, buf+i, SLEN);
    }
}


void runforward(const char *fwdname, FILE *mfile, const char *uname, 
                const char *homedir, char *cname, int send)
{
    FILE *fwd;
    char buf[SLEN];
    int blen;
    int status;
    char *sargv[SLEN];
    int sargc;
    char scriptname[SLEN];

    fwd = fopen(fwdname, "r");
    if(fwd == NULL) {
        exit(1);
    }

    sargc = 1;
    while(!feof(fwd)) {
        fseek(mfile, 0, SEEK_SET);
        if(fgets(buf, SLEN, fwd) == NULL) {
            break;
        }
//        printf("buf: |%s|\n", buf);
        /* Ignore comment lines */
        if(buf[0] == '#') {
            continue;
        }
        blen = strlen(buf);
        if(buf[blen-1] == '\n') {
            buf[blen-1] = '\0';
        }
        eat_wspace(buf);
        /* Simple unquoting */
        if(buf[0] == '"' || buf[0] == '\'') {
            char *end;

            end = strrchr(buf, buf[0]);
            if(end <= buf) {
                exit(0);
            }
            strncpy(buf, buf+1, end-buf-1);
            buf[end-buf-1] = '\0';
        }
        eat_wspace(buf);
//        printf("unquoted: ***%s***\n", buf);
        if(buf[0] == '\\') {
            strncpy(buf, buf+1, SLEN);
        }
        if(buf[0] == '|') {
            char *argv[SLEN];
            char *tok;
            int i;

            i = 0;
            tok = strtok(buf+1, " \n");
            argv[0] = tok;
            while(tok != NULL) {
                i++;
                tok = strtok(NULL, " \n");
                argv[i] = tok;
            }
            runprog(argv, mfile, homedir);
        } else if(buf[0] == '/') {
            mboxmail(mfile, buf, cname);
        } else if(strchr(buf, '@')) {
            if(send) {
                sargv[sargc] = (char *) malloc(strlen(buf)+1);
                if(sargv[sargc] != NULL) {
                    strcpy(sargv[sargc], buf);
                    sargc++;
                } else {
                    exit(1);
                }
            } else {
                printf("%s\n", buf);
            }
        } else if(strncmp(uname, buf, SLEN) == 0) {
                char mboxname[SLEN];

                snprintf(mboxname, SLEN, "%s/%s", MAILDIR, uname);
                mboxmail(mfile, mboxname, cname); // FIXME: check retval
        } else {
            printf("%s\n", buf);
        }
    }
    fclose(fwd);

    if(send) {
        snprintf(scriptname, SLEN-1, "%s/%s", SCRIPTDIR, "queue-mail");
        sargv[0] = scriptname;
        sargv[sargc] = NULL;
        status = runprog(sargv, mfile, homedir);
        if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            exit(66);
        }
        // FIXME: Maybe run free() on sargv[>0]
    }
    exit(0);
}


int main(int argc, char *argv[])
{
    struct passwd *userinfo;
    struct passwd *callerinfo;
    uid_t uid, cuid;
    gid_t gid;
    char *uname, *cmd;
    char cname[SLEN];
    char shell[SLEN];
    char homedir[SLEN];
    char fwdname[SLEN];

    if(argc < 3) {
        exit(1);
    }
    cmd = argv[1];
    uname = argv[2];
#if DENY_ROOT
    if(!strncmp(uname, "root", 5)) {
        exit(2);
    }
#endif
    userinfo = getpwnam(uname);
    if(!userinfo) {
        exit(3);
    }
    uid = userinfo->pw_uid;
#if DENY_ROOT
    if(uid < 1000) {
#else
    if((uid < 1000) && (uid != 0)) {
#endif
        exit(2);
    }
    gid = userinfo->pw_gid;
    strncpy(shell, userinfo->pw_shell, SLEN-1);
    shell[SLEN-1] = '\0';
    strncpy(homedir, userinfo->pw_dir, SLEN-1);
    homedir[SLEN-1] = '\0';
    /* Check if user is ok to receive mail */
    checkshell(shell);

    cuid = getuid();
    callerinfo = getpwuid(cuid);
    if(!callerinfo) {
        exit(3);
    }
    strncpy(cname, callerinfo->pw_name, SLEN-1);
    touchmbox(uname, uid);
    /* Drop privs */
    if(setuid(uid) != 0) {
        exit(4);
    }
    
    if(!strncmp(cmd, "deliver", 7) || !strncmp(cmd, "forward", 7)) {
        int send, status, c;
        FILE *mfile;
        int statret;
        struct stat ss;

        snprintf(fwdname, SLEN-1, "%s/mailscript/git-mailscript/forward", homedir);
        statret = stat(fwdname, &ss);
        if((statret != 0) || ((ss.st_uid != uid) && (ss.st_uid != 0))) {
            /* .forward does not exist or not owned by user or root */
            char mboxname[SLEN];

            snprintf(mboxname, SLEN, "%s/%s", MAILDIR, uname);
            return mboxmail(stdin, mboxname, cname);    
        }
        send = 0;
        if(!strncmp(cmd, "forward", 7)) {
            char *sargv[SLEN];
            char scriptname[SLEN];

            /* Check if we can send external mail */
            snprintf(scriptname, SLEN-1, "%s/%s", SCRIPTDIR, "can-send");
            sargv[0] = scriptname;
            sargv[1] = NULL;
            status = runprog(sargv, stdin, homedir);
            if(WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                send = 1;
            }
        }
        mfile = tmpfile();
        if(mfile == NULL) {
            return 1;
        }
        c = getc(stdin);
        while(!feof(stdin)) {
            putc(c, mfile);
            c = getc(stdin);
        }
        if(ferror(mfile)) {
            /* some error */
            return 1;
        }
        runforward(fwdname, mfile, uname, homedir, cname, send);
    } else if(!strncmp(cmd, "send", 5)) {
        ;
    } else {
        printf("Unknown command\n");
    }

    return 66;
}
