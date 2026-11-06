// Command rules of Agent Access (core/AgentAccess): the real CommandAllowed
// against every rule bypass found so far, then a differential fuzz: random
// shell-like text the checker accepts is run by real shells, with a canary
// program that must never start and a folder that must stay unchanged.
// Also the audit log, recordings, the socket location, downloads and frames.
//
// NIXSCP_RULES_FUZZ=<cases> changes the number of fuzz cases (default 1500).
#include "core/AgentAccess.h"
#include "core/CloudCrypto.h"
#include "core/ConfigVault.h"
#include "core/Configuration.h"
#include "core/SessionData.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>

using namespace AgentAccess;

static int Failures = 0;

#define CHECK(Cond) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); std::fflush(stdout); Failures++; } } while (0)
#define CHECK_MSG(Cond, Msg) \
  do { if (!(Cond)) { std::printf("FAIL %s:%d: %s\n  %s\n", __FILE__, __LINE__, #Cond, qPrintable(Msg)); std::fflush(stdout); Failures++; } } while (0)

static void Refused(const SessionData & D, const QString & Command, const char * Why = nullptr)
{
  QString Reason;
  CHECK_MSG(!CommandAllowed(D, Command, &Reason), QStringLiteral("allowed: %1").arg(VisibleText(Command, false)));
  CHECK_MSG(!Reason.isEmpty(), QStringLiteral("no reason for: %1").arg(VisibleText(Command, false)));
  if (Why != nullptr)
  {
    CHECK_MSG(Reason.contains(QLatin1String(Why)), QStringLiteral("%1: %2").arg(VisibleText(Command, false), Reason));
  }
}

static void Allowed(const SessionData & D, const QString & Command)
{
  QString Reason;
  CHECK_MSG(CommandAllowed(D, Command, &Reason), QStringLiteral("refused: %1: %2").arg(VisibleText(Command, false), Reason));
}

static SessionData ReadOnly()
{
  SessionData D;
  D.AgentAllow = ReadOnlyAllowPreset();
  D.AgentDeny = ReadOnlyDenyPreset();
  return D;
}

// The bypasses of the 2026-11-05 audit, as they were reported
static void TestAuditBypasses()
{
  SessionData P = ReadOnly();
  // S30, X-H5: quoting the old splitter misread hid the rm from it
  Refused(P, QStringLiteral("echo $'\\'' ; rm -rf /tmp/victim ; echo $'\\''"));
  Refused(P, QStringLiteral("ls # '\nrm -rf /tmp/victim\n#'"), "Comments");
  Refused(P, QStringLiteral("ls # \"\nrm -rf /tmp/victim\n#\""), "Comments");
  // X-M6: >& is a file write unless a descriptor follows
  Refused(P, QStringLiteral("echo hi >& /tmp/x"), "file");
  Refused(P, QStringLiteral("echo hi >&/tmp/x"), "file");
  Allowed(P, QStringLiteral("echo hi >&2"));
  Allowed(P, QStringLiteral("echo hi 2>&1"));
  Allowed(P, QStringLiteral("ls 2>&-"));
  // L3: writers in the preset
  Refused(P, QStringLiteral("sort -o /etc/passwd /tmp/x"));
  Refused(P, QStringLiteral("find / -fls /tmp/out"));
  Refused(P, QStringLiteral("date -s 2000-01-01"));
  // X-M7: NUL ended the string the server got, after the checker saw more
  SessionData Suffix;
  Suffix.AgentAllow = QStringLiteral("rm *.tmp");
  QString Nul = QStringLiteral("rm /etc/passwd");
  Nul += QChar(0);
  Nul += QStringLiteral(".tmp");
  Refused(Suffix, Nul, "NUL");
  SessionData Exact;
  Exact.AgentDeny = QStringLiteral("reboot");
  QString Nul2 = QStringLiteral("reboot");
  Nul2 += QChar(0);
  Nul2 += QStringLiteral("x");
  Refused(Exact, Nul2, "NUL");
  // without any rules too: what was checked, shown and logged must be what runs
  Refused(SessionData(), Nul, "NUL");
  // X-M8: quoting does not change what runs
  Refused(P, QStringLiteral("find /srv \"-delete\""));
  Refused(P, QStringLiteral("find /srv '-exec' rm {} +"));
  Refused(P, QStringLiteral("find /srv -d\\elete"));
  Refused(P, QStringLiteral("find /srv -\"d\"el'e'te"));
  // L1: a denied pattern that does not compile blocks instead of denying nothing
  SessionData Bad;
  Bad.AgentDeny = QStringLiteral("rm [*\nshutdown[");
  Refused(Bad, QStringLiteral("rm [foo"), "not valid");
  Refused(Bad, QStringLiteral("shutdown["), "not valid");
  Refused(Bad, QStringLiteral("ls"), "not valid");
  CHECK(InvalidPattern(Bad.AgentDeny) == QStringLiteral("rm [*"));
  CHECK(InvalidPattern(QStringLiteral("rm *\nls [a-z]*")).isEmpty());
  // q.cpp of the review
  Refused(P, QStringLiteral("ip addr add 1.2.3.4/24 dev eth0"));
  Refused(P, QStringLiteral("hostname evil"));
  Refused(P, QStringLiteral("git diff --output=/tmp/x"));
}

// What the checker must refuse because it cannot be sure of it
static void TestFailClosed()
{
  SessionData P = ReadOnly();
  SessionData Deny;
  Deny.AgentDeny = QStringLiteral("rm *\nreboot*");
  for (const SessionData & D : {P, Deny})
  {
    // here-documents and here-strings
    Refused(D, QStringLiteral("cat <<EOF\nhello\nEOF"), "Here-documents");
    Refused(D, QStringLiteral("cat <<-EOF\nhello\nEOF"), "Here-documents");
    Refused(D, QStringLiteral("cat <<'X'\n'\nrm -rf /\n#\nX"), "Here-documents");
    Refused(D, QStringLiteral("cat <<<hello"), "Here-documents");
    // line continuations
    Refused(D, QStringLiteral("ls \\\n-la"), "continuation");
    Refused(D, QStringLiteral("ec\\\nho hi"), "continuation");
    Refused(D, QStringLiteral("echo \"a\\\nb\""), "continuation");
    // odd quoting
    Refused(D, QStringLiteral("echo 'a"), "unterminated");
    Refused(D, QStringLiteral("echo \"a"), "unterminated");
    Refused(D, QStringLiteral("echo a\\"), "backslash");
    Refused(D, QStringLiteral("echo $'a'"), "$'...'");
    Refused(D, QStringLiteral("echo $\"a\""), "$'...'");
    // what only the server knows
    Refused(D, QStringLiteral("echo $(id)"), "substitution");
    Refused(D, QStringLiteral("echo `id`"), "substitution");
    Refused(D, QStringLiteral("echo \"$(id)\""), "substitution");
    Refused(D, QStringLiteral("echo \"`id`\""), "substitution");
    Refused(D, QStringLiteral("echo $HOME"), "Variables");
    Refused(D, QStringLiteral("echo \"$HOME\""), "Variables");
    Refused(D, QStringLiteral("echo ${HOME}"), "Variables");
    Refused(D, QStringLiteral("find /srv $IFS-delete"), "Variables");
    Refused(D, QStringLiteral("find /srv ${IFS}-delete"), "Variables");
    Refused(D, QStringLiteral("$0 -c id"), "Variables");
    Refused(D, QStringLiteral("echo $[1+1]"), "Variables");
    Refused(D, QStringLiteral("echo $?"), "Variables");
    Refused(D, QStringLiteral("echo $$"), "Variables");
    Refused(D, QStringLiteral("/bin/r? -rf /"), "wildcard");
    Refused(D, QStringLiteral("find /srv -dele[t]e"), "wildcard");
    Refused(D, QStringLiteral("find /srv -del*"), "wildcard");
    // grammar that hides commands
    Refused(D, QStringLiteral("(rm -rf /tmp/x)"), "Subshells");
    Refused(D, QStringLiteral("ls; (rm -rf /tmp/x)"), "Subshells");
    Refused(D, QStringLiteral("cat <(rm -rf /tmp/x)"), "Process substitution");
    Refused(D, QStringLiteral("echo hi > >(rm -rf /tmp/x)"), "Process substitution");
    Refused(D, QStringLiteral("f() { rm -rf /tmp/x; }; f"), "Subshells");
    Refused(D, QStringLiteral("{ rm -rf /tmp/x; }"), "Braces");
    Refused(D, QStringLiteral("{rm,-rf,/tmp/x}"), "Braces");
    Refused(D, QStringLiteral("find /srv -{delete,print}"), "Braces");
    Refused(D, QStringLiteral("if true; then rm -rf /tmp/x; fi"), "keywords");
    Refused(D, QStringLiteral("for f in x; do rm -rf /tmp/x; done"), "keywords");
    Refused(D, QStringLiteral("while true; do rm x; done"), "keywords");
    Refused(D, QStringLiteral("case x in x) rm y;; esac"));
    Refused(D, QStringLiteral("! rm -rf /tmp/x"), "keywords");
    Refused(D, QStringLiteral("time rm -rf /tmp/x"), "keywords");
    Refused(D, QStringLiteral("[[ -f x ]] && ls"), "keywords");
    Refused(D, QStringLiteral("X=1 rm -rf /tmp/x"), "variables");
    Refused(D, QStringLiteral("LD_PRELOAD=/tmp/x.so ls"), "variables");
    Refused(D, QStringLiteral("PATH=/tmp ls"), "variables");
    Refused(D, QStringLiteral("GIT_PAGER=evil git log"), "variables");
    Refused(D, QStringLiteral("a+=b ls"), "variables");
    Refused(D, QStringLiteral("echo hi #comment"), "Comments");
    Refused(D, QStringLiteral("ls;#comment"), "Comments");
    // control characters
    Refused(D, QStringLiteral("ls\rrm x"), "Control");
    Refused(D, QStringLiteral("ls\x1b[2Jx"), "Control");
    // a redirection needs its target
    Refused(D, QStringLiteral("ls >"), "target");
    Refused(D, QStringLiteral("ls > ; rm x"));
    Refused(D, QStringLiteral("ls >\nrm x"));
    // nothing to run
    Refused(D, QStringLiteral(""), "Empty");
    Refused(D, QStringLiteral("   \n "), "Empty");
    Refused(D, QStringLiteral(";"), "Empty");
    // too long to be worth checking
    Refused(D, QStringLiteral("ls ") + QString(70000, QLatin1Char('a')), "too long");
  }
  // what shells disagree on. bash: arguments of ls. dash: a new command
  Refused(P, QStringLiteral("ls &>/dev/null rm x"), "different shells");
  Refused(P, QStringLiteral("ls &>>/dev/null rm x"), "different shells");
  Refused(P, QStringLiteral("ls &> /dev/null 2>&1 rm x"), "different shells");
  Refused(Deny, QStringLiteral("ls &>/dev/null rm x"), "different shells");
  Allowed(P, QStringLiteral("ls -la &>/dev/null"));
  Allowed(P, QStringLiteral("ls &>/dev/null; uptime"));
  // only one digit before > is a descriptor everywhere; more are an argument
  Refused(P, QStringLiteral("date 010100002000>/dev/null"), "not among");
  Refused(P, QStringLiteral("hostname 12>/dev/null"), "not among");
  Allowed(P, QStringLiteral("date 2>/dev/null"));
  // zsh
  Refused(Deny, QStringLiteral("repeat 3 rm x"), "keywords");
  Refused(Deny, QStringLiteral("noglob rm x"), "denied");
  Refused(Deny, QStringLiteral("ls *(e:'rm x':)"), "Subshells");
  Refused(Deny, QStringLiteral("ls <1-10>"), "target");

  // text that only looks like shell syntax is fine
  Allowed(P, QStringLiteral("echo '$(id)' '`id`' '#x' '<<EOF' '$HOME'"));
  Allowed(P, QStringLiteral("echo \"a b\" 'c;d' e\\;f"));
  Allowed(P, QStringLiteral("echo a#b"));
  Allowed(P, QStringLiteral("echo foo$"));
  Allowed(P, QStringLiteral("echo \"foo$\""));
  Allowed(P, QStringLiteral("echo \\$HOME"));
  Allowed(P, QStringLiteral("echo \"\\$HOME\""));
  Allowed(P, QStringLiteral("grep 'a$' /etc/hosts"));
  Allowed(P, QStringLiteral("echo '{a,b}' \\{a,b\\}"));
  Allowed(P, QStringLiteral("echo '\n'"));
}

static void TestDenyRules()
{
  SessionData D;
  D.AgentDeny = QStringLiteral("rm *\nreboot*\n* /etc/shadow*");
  // quoting, a path or a redirection in front do not change the program
  Refused(D, QStringLiteral("rm -rf /"), "denied");
  Refused(D, QStringLiteral("\"rm\" -rf /"), "denied");
  Refused(D, QStringLiteral("r\\m -rf /"), "denied");
  Refused(D, QStringLiteral("r'm' -rf /"), "denied");
  Refused(D, QStringLiteral("/bin/rm -rf /"), "denied");
  Refused(D, QStringLiteral("./rm -rf /"), "denied");
  Refused(D, QStringLiteral("</dev/null rm -rf /"), "denied");
  Refused(D, QStringLiteral(">/dev/null rm -rf /"), "denied");
  Refused(D, QStringLiteral("2>/dev/null rm -rf /"), "denied");
  Refused(D, QStringLiteral("ls; rm -rf /"), "denied");
  Refused(D, QStringLiteral("ls && rm -rf /"), "denied");
  Refused(D, QStringLiteral("ls | rm -rf /"), "denied");
  Refused(D, QStringLiteral("ls |& rm -rf /"), "denied");
  Refused(D, QStringLiteral("ls & rm -rf /"), "denied");
  Refused(D, QStringLiteral("ls\nrm -rf /"), "denied");
  Refused(D, QStringLiteral("rm \"-rf\" \"/tmp/a\nb\""), "denied");
  // case-insensitive file systems
  Refused(D, QStringLiteral("RM -rf /"), "denied");
  // programs that start the program named in their arguments
  Refused(D, QStringLiteral("sudo rm -rf /"), "denied");
  Refused(D, QStringLiteral("sudo -u root rm -rf /"), "denied");
  Refused(D, QStringLiteral("/usr/bin/sudo /bin/rm -rf /"), "denied");
  Refused(D, QStringLiteral("env rm -rf /"), "denied");
  Refused(D, QStringLiteral("env A=1 rm -rf /"), "denied");
  Refused(D, QStringLiteral("nice -n 5 rm x"), "denied");
  Refused(D, QStringLiteral("nohup rm x"), "denied");
  Refused(D, QStringLiteral("timeout 5 rm x"), "denied");
  Refused(D, QStringLiteral("command rm x"), "denied");
  Refused(D, QStringLiteral("exec rm x"), "denied");
  Refused(D, QStringLiteral("busybox rm x"), "denied");
  Refused(D, QStringLiteral("xargs rm"), "denied");
  Refused(D, QStringLiteral("find . -exec rm {} ;"), "denied");
  Refused(D, QStringLiteral("find . -exec sudo rm {} ;"), "denied");
  Refused(D, QStringLiteral("find . -exec sh -c x ;"), "cannot check");
  Allowed(D, QStringLiteral("find . -name sh -print"));
  Allowed(D, QStringLiteral("find . -name rm"));
  Refused(D, QStringLiteral("su -c 'rm x' root"), "cannot check");
  // what runs shell text cannot be checked at all
  Refused(D, QStringLiteral("eval rm x"), "cannot check");
  Refused(D, QStringLiteral("sh -c 'rm x'"), "cannot check");
  Refused(D, QStringLiteral("bash -c 'rm x'"), "cannot check");
  Refused(D, QStringLiteral("/bin/bash script"), "cannot check");
  Refused(D, QStringLiteral("sudo bash -c 'rm x'"), "cannot check");
  Refused(D, QStringLiteral(". /tmp/x"), "cannot check");
  Refused(D, QStringLiteral("source /tmp/x"), "cannot check");
  Refused(D, QStringLiteral("alias ls=rm"), "cannot check");
  Refused(D, QStringLiteral("trap 'rm x' EXIT"), "cannot check");
  // a redirection's file counts too
  Refused(D, QStringLiteral("cat /etc/shadow"), "denied");
  Refused(D, QStringLiteral("cat </etc/shadow"), "denied");
  Refused(D, QStringLiteral("cat < '/etc/shadow'"), "denied");
  // and everything else passes
  Allowed(D, QStringLiteral("ls -la"));
  Allowed(D, QStringLiteral("echo rm -rf"));
  Allowed(D, QStringLiteral("touch /tmp/x; echo x > /tmp/y"));
  Allowed(D, QStringLiteral("cd /tmp && ls"));
  // denied patterns may name raw syntax
  SessionData Pipes;
  Pipes.AgentDeny = QStringLiteral("*|*");
  Refused(Pipes, QStringLiteral("ls | wc -l"), "denied");
  Allowed(Pipes, QStringLiteral("ls"));
  // deny beats allow
  SessionData Both;
  Both.AgentAllow = QStringLiteral("git *");
  Both.AgentDeny = QStringLiteral("git push*");
  Allowed(Both, QStringLiteral("git status"));
  Refused(Both, QStringLiteral("git push origin"), "denied");
  Refused(Both, QStringLiteral("git \"push\" origin"), "denied");
}

static void TestAllowRules()
{
  SessionData D;
  D.AgentAllow = QStringLiteral("ls*\nuptime\necho *\nbash /opt/check.sh");
  // quoting removed before matching, redirections left out of it
  Allowed(D, QStringLiteral("\"ls\" -la"));
  Allowed(D, QStringLiteral("uptime 2>/dev/null"));
  Allowed(D, QStringLiteral("uptime &>/dev/null"));
  Allowed(D, QStringLiteral("uptime </dev/null"));
  Refused(D, QStringLiteral("cat /etc/hostname"), "not among");
  Refused(D, QStringLiteral("uptime; id"), "not among");
  Refused(D, QStringLiteral("> /tmp/x"), "file");
  Refused(D, QStringLiteral("< /etc/passwd"), "not among");
  // every way of writing a file
  for (const char * Write : {"echo x > /tmp/x", "echo x >> /tmp/x", "echo x >| /tmp/x", "echo x <> /tmp/x", "echo x 2> /tmp/x",
    "echo x &> /tmp/x", "echo x &>> /tmp/x", "echo x >& /tmp/x", "echo x >\"/tmp/x\"", "echo x > '/dev/null2'", "> /tmp/x echo x"})
  {
    Refused(D, QString::fromLatin1(Write), "file");
  }
  Allowed(D, QStringLiteral("echo x > \"/dev/null\""));
  Allowed(D, QStringLiteral("echo x >&/dev/null"));
  // a pattern naming a shell allows that shell, a bare * does not
  Allowed(D, QStringLiteral("bash /opt/check.sh"));
  SessionData Star;
  Star.AgentAllow = QStringLiteral("*");
  Star.AgentDeny = QStringLiteral("rm *");
  Allowed(Star, QStringLiteral("cat /etc/hostname"));
  Refused(Star, QStringLiteral("eval rm x"), "cannot check");
  Refused(Star, QStringLiteral("sh -c 'rm x'"), "cannot check");
  // an invalid allowed pattern only never matches
  SessionData Bad;
  Bad.AgentAllow = QStringLiteral("ls[\nuptime");
  Refused(Bad, QStringLiteral("ls"), "not among");
  Allowed(Bad, QStringLiteral("uptime"));
  CHECK(InvalidPattern(Bad.AgentAllow) == QStringLiteral("ls["));

  // explicitly allowed: a pattern without wildcards that is the whole line
  SessionData Explicit;
  Explicit.AgentAllow = QStringLiteral("echo *\necho $HOME\nls | wc -l\ncat /etc/motd # note");
  Allowed(Explicit, QStringLiteral("echo $HOME"));
  Allowed(Explicit, QStringLiteral("  echo $HOME\n"));
  Allowed(Explicit, QStringLiteral("cat /etc/motd # note"));
  Refused(Explicit, QStringLiteral("echo $HOME/x"), "Variables");
  Refused(Explicit, QStringLiteral("echo $PATH"), "Variables");
  // a wildcard pattern never unlocks what could not be read
  Refused(Explicit, QStringLiteral("echo $(id)"), "substitution");
  // not even a whole line gets past a denied pattern
  Explicit.AgentDeny = QStringLiteral("*HOME*");
  Refused(Explicit, QStringLiteral("echo $HOME"), "denied");
}

static void TestReadOnlyPreset()
{
  SessionData P = ReadOnly();
  CHECK(InvalidPattern(P.AgentAllow).isEmpty() && InvalidPattern(P.AgentDeny).isEmpty());
  for (const char * Command : {
    "ls", "ls -la /var/www", "pwd", "whoami", "id", "id -u", "cat /etc/hostname", "head -n 5 /etc/passwd", "tail -n 100 /var/log/syslog",
    "df -h && free -m && uptime", "uname -a", "tail -n 100 /var/log/syslog | grep error", "grep -rn 'a;b' /etc", "wc -l /etc/passwd",
    "ls | wc -l", "ls | wc", "stat /etc/passwd", "file -b /bin/ls", "du -sh /var/log/*", "find /var/log -name '*.gz' -mtime +7",
    "find . -name x -print", "ps", "ps aux | sort -rnk3 | head -5", "cut -d: -f1 /etc/passwd | sort | uniq -c | sort -rn",
    "date", "date +%s", "date -u +%F", "date '+%Y-%m-%d %H:%M'", "date -u", "date -R", "hostname", "hostname -f", "top -bn1", "top -bn1 -o %CPU",
    "lsblk", "lsblk -f", "ss -tlnp", "netstat -tlnp", "ip a", "ip addr", "ip -br a", "ip a show dev lo", "ip r", "ip route show", "ip route get 1.1.1.1",
    "ip link", "systemctl status nginx --no-pager", "systemctl is-active nginx", "systemctl is-enabled nginx", "systemctl list-units --type=service",
    "systemctl --failed", "journalctl", "journalctl -u nginx -n 100 --no-pager --since '1 hour ago'", "journalctl -xe -r --until now",
    "docker ps", "docker ps -a", "docker logs web", "docker images", "docker inspect web", "git status", "git status --short", "git log --oneline -20",
    "git diff --stat", "git diff HEAD~1", "nginx -t", "apachectl -t", "echo", "echo hello", "which git", "ls 2>/dev/null", "ls &>/dev/null"})
  {
    Allowed(P, QString::fromLatin1(Command));
  }
  for (const char * Command : {
    // write a file
    "sort -o /tmp/x /etc/passwd", "sort --output=/tmp/x /etc/passwd", "sort --outp=/tmp/x /etc/passwd", "sort -ro /tmp/x /etc/passwd",
    "sort -bdfro /tmp/x /etc/passwd", "sort /etc/passwd -o /tmp/x", "sort -n -o /tmp/x", "sort -n /etc/passwd -o/tmp/x",
    "uniq /etc/passwd /tmp/x", "uniq -c /etc/passwd /tmp/x",
    "find / -fls /tmp/x", "find / -fprint /tmp/x", "find / -fprint0 /tmp/x", "find / -fprintf /tmp/x %p", "find -delete", "find . -delete -print",
    "git diff --output=/tmp/x", "git log --output=/tmp/x", "git log -p --outpu=/tmp/x", "git diff --ou=/tmp/x",
    "file -C -m /tmp/x", "file -bC -m /tmp/x", "file --compile -m /tmp/x", "file --comp -m /tmp/x",
    "ss -D /tmp/x", "ss -tD /tmp/x", "ss --diag=/tmp/x", "journalctl --cursor-file=/tmp/x", "journalctl --cursor-f=/tmp/x",
    // change the system
    "date -s 2000-01-01", "date --set=2000-01-01", "date --set 2000-01-01", "date --s=2000", "date -us 2000-01-01", "date -uRs 2000", "date 010100002000",
    "date -u 010100002000", "date +%s -s 2000", "date -u +%s --set=2000", "date +%s -us 2000",
    "hostname evil", "hostname -F /tmp/x", "hostname -b evil", "hostname -f evil",
    "ip addr add 1.2.3.4/24 dev eth0", "ip a add 1.2.3.4/24 dev eth0", "ip a del 1.2.3.4/24 dev eth0", "ip a flush dev eth0",
    "ip route add default via 1.2.3.4", "ip r del default", "ip r flush table main", "ip rule add from all table 1", "ip link set eth0 down",
    "ss -K", "ss -tK", "ss --kill", "journalctl --rotate", "journalctl --rot", "journalctl --vacuum-size=1", "journalctl --vacuum-time=1s",
    "journalctl --vacuum-files=1", "journalctl --va=1", "journalctl --flush", "journalctl --flu", "journalctl --sync", "journalctl --relinquish-var",
    "journalctl --smart-relinquish-var", "journalctl --setup-keys", "journalctl --update-catalog", "journalctl -n 5 --rotate",
    "systemctl restart nginx", "systemctl stop nginx", "systemctl --failed restart nginx", "systemctl --failed stop nginx", "systemctl list-unit-files; reboot",
    // run other programs
    "sort --compress-program=/tmp/x /etc/passwd", "sort --compress-prog=/tmp/x", "find / -exec rm {} ;", "find / -execdir rm {} ;",
    "find / -ok rm {} ;", "find / -okdir rm {} ;", "git log --ext-diff", "git diff --ext-diff", "git diff --ext",
    // not on the list
    "rm -rf /", "cp a b", "mv a b", "tee /tmp/x", "touch /tmp/x", "lsof", "lsh", "dfx", "freecad", "uptimex", "unamex", "idle", "datefudge",
    "docker psx", "git statusx", "git logx", "git diffx", "docker run x", "git push", "git -c core.pager=x log", "sh", "bash", "python3 -c x"})
  {
    Refused(P, QString::fromLatin1(Command));
  }
}

// Random shell-like text. Whatever the checker accepts under "probe" rules runs
// in a real shell: the canary must not start, and no file may appear or change
static void DifferentialFuzz(const QString & Shell, const QString & Dir, int Cases, quint32 Seed)
{
  QString Canary = Dir + QStringLiteral("/canary-ran");
  QString Work = Dir + QStringLiteral("/work");
  static const QStringList Tokens{
    QStringLiteral("probe"), QStringLiteral("probe"), QStringLiteral("probe"), QStringLiteral("canary"), QStringLiteral("canary"),
    QStringLiteral(" "), QStringLiteral(" "), QStringLiteral(" "), QStringLiteral("\t"), QStringLiteral("\n"), QStringLiteral(";"),
    QStringLiteral("&"), QStringLiteral("&&"), QStringLiteral("|"), QStringLiteral("||"), QStringLiteral("|&"), QStringLiteral("'"),
    QStringLiteral("\""), QStringLiteral("\\"), QStringLiteral("$"), QStringLiteral("$'"), QStringLiteral("$\""), QStringLiteral("("),
    QStringLiteral(")"), QStringLiteral("`"), QStringLiteral("#"), QStringLiteral("<"), QStringLiteral(">"), QStringLiteral(">>"),
    QStringLiteral(">&"), QStringLiteral("&>"), QStringLiteral("<&"), QStringLiteral("<>"), QStringLiteral(">|"), QStringLiteral("<<"),
    QStringLiteral("<<<"), QStringLiteral("{"), QStringLiteral("}"), QStringLiteral("{}"), QStringLiteral("*"), QStringLiteral("?"),
    QStringLiteral("["), QStringLiteral("]"), QStringLiteral("~"), QStringLiteral("="), QStringLiteral("!"), QStringLiteral("%"),
    QStringLiteral("^"), QStringLiteral(","), QStringLiteral("."), QStringLiteral(".."), QStringLiteral("/"), QStringLiteral("-"),
    QStringLiteral("--"), QStringLiteral("a"), QStringLiteral("x"), QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("f"),
    QStringLiteral("EOF"), QStringLiteral("eval"), QStringLiteral("sh"), QStringLiteral("if"), QStringLiteral("then"), QStringLiteral("fi"),
    QStringLiteral("do"), QStringLiteral("done"), QStringLiteral("for"), QStringLiteral("case"), QStringLiteral("esac"), QStringLiteral("IFS"),
    QStringLiteral("HOME"), QStringLiteral("/dev/null"), QStringLiteral("newfile"), QStringLiteral("\\\n"), QStringLiteral("\\;"),
    QStringLiteral("\\'"), QStringLiteral("'canary'"), QStringLiteral("\"canary\""), QStringLiteral(" canary"), QStringLiteral("; canary"),
    QStringLiteral("\ncanary"), QStringLiteral("|canary"), QStringLiteral("&canary"), QStringLiteral("$(canary)"), QStringLiteral("`canary`"),
    QStringLiteral("&>/dev/null"), QStringLiteral("&>/dev/null canary"), QStringLiteral("2>/dev/null"), QStringLiteral("12>/dev/null"),
    QStringLiteral(">&2"), QStringLiteral("<&-"), QStringLiteral("<&0"), QStringLiteral(" # "), QStringLiteral("\\#"), QStringLiteral("''"), QStringLiteral("\"\""),
    // what the conversion to bytes takes away must not hide what is around it
    QString(QChar(0xD800)), QString(QChar(0xDC00)), QString(QChar(0xDFFF)), QStringLiteral("$") + QChar(0xD800) + QStringLiteral("(canary)"),
    QStringLiteral("`") + QChar(0xD800) + QStringLiteral("canary`"), QString(QChar(0xFEFF)), QStringLiteral("\u00a0"), QStringLiteral("\u2028"), QStringLiteral("\u0085")};
  SessionData D;
  D.AgentAllow = QStringLiteral("probe\nprobe *");
  QRandomGenerator Random(Seed);
  QProcessEnvironment Environment;
  Environment.insert(QStringLiteral("PATH"), Dir + QStringLiteral("/bin"));
  Environment.insert(QStringLiteral("HOME"), Work);
  int Accepted = 0;
  for (int Case = 0; Case < Cases; Case++)
  {
    QString Command = QStringLiteral("probe ");
    int Count = 1 + static_cast<int>(Random.bounded(10u));
    for (int I = 0; I < Count; I++)
    {
      Command += Tokens.at(static_cast<qsizetype>(Random.bounded(static_cast<quint32>(Tokens.size()))));
    }
    if (!CommandAllowed(D, Command))
    {
      continue;
    }
    Accepted++;
    QProcess P;
    P.setProcessEnvironment(Environment);
    P.setWorkingDirectory(Work);
    P.setStandardErrorFile(QProcess::nullDevice());
    P.setStandardOutputFile(QProcess::nullDevice());
    P.setStandardInputFile(QProcess::nullDevice());
    // what the server would get
    P.start(Shell, {QStringLiteral("-c"), QString::fromUtf8(Command.toUtf8())});
    if (!P.waitForFinished(3000))
    {
      P.kill();
      P.waitForFinished(1000);
    }
    QStringList Files = QDir(Work).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    CHECK_MSG(!QFile::exists(Canary), QStringLiteral("%1 ran the canary: %2").arg(Shell, VisibleText(Command, false)));
    CHECK_MSG(Files == QStringList{QStringLiteral("f")}, QStringLiteral("%1 changed the folder (%2): %3").arg(Shell, Files.join(QLatin1Char(',')), VisibleText(Command, false)));
    if (QFile::exists(Canary) || (Files != QStringList{QStringLiteral("f")}))
    {
      // the next cases should start clean
      QFile::remove(Canary);
      for (const QString & File : Files)
      {
        if (File != QStringLiteral("f"))
        {
          QFile::remove(Work + QLatin1Char('/') + File);
        }
      }
    }
  }
  std::printf("fuzz: %s %d cases, %d accepted and run\n", qPrintable(Shell), Cases, Accepted);
  // the fuzz only proves something when plenty of cases get through
  CHECK(Accepted > Cases / 20);
}

static void TestDifferential()
{
  QTemporaryDir Dir;
  QDir(Dir.path()).mkpath(QStringLiteral("bin"));
  QDir(Dir.path()).mkpath(QStringLiteral("work"));
  auto Script = [&Dir](const QString & Name, const QByteArray & Body)
  {
    QFile F(Dir.path() + QStringLiteral("/bin/") + Name);
    CHECK(F.open(QIODevice::WriteOnly) && (F.write(Body) == Body.size()));
    F.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  };
  // /bin/sh by full path: PATH of the fuzz has this folder only
  Script(QStringLiteral("probe"), "#!/bin/sh\nexit 0\n");
  Script(QStringLiteral("canary"), "#!/bin/sh\n: > '" + Dir.path().toUtf8() + "/canary-ran'\n");
  {
    QFile F(Dir.path() + QStringLiteral("/work/f"));
    CHECK(F.open(QIODevice::WriteOnly));
  }
  // attached options with the real programs: a canary that is denied must not run
  {
    SessionData D;
    D.AgentDeny = QStringLiteral("canary\ncanary *");
    QProcessEnvironment Environment;
    Environment.insert(QStringLiteral("PATH"), Dir.path() + QStringLiteral("/bin"));
    QString Bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
    for (const char * Line : {"/usr/bin/env -S'canary x'", "/usr/bin/env -S 'canary x'", "/usr/bin/env --split-string='canary x'", "/usr/bin/env -vS'canary x'",
      "/usr/bin/flock f -c 'true; canary'", "/usr/bin/flock f --command 'true; canary'",
      "let 'x[$(canary)]=1'", "declare 'a[$(canary)]=1'", "typeset 'a[$(canary)]=1'", "printf -v 'a[$(canary)]' x", "test -v 'a[$(canary)]'",
      "compgen -C canary", "compgen -W '$(canary)'"})
    {
      QString Command = QString::fromUtf8(Line);
      QString Canary = Dir.path() + QStringLiteral("/canary-ran");
      QFile::remove(Canary);
      bool Builtin = !Command.startsWith(QLatin1Char('/'));
      if (Builtin ? !Bash.isEmpty() : QFile::exists(Command.section(QLatin1Char(' '), 0, 0)))
      {
        QProcess P;
        P.setProcessEnvironment(Environment);
        P.setWorkingDirectory(Dir.path() + QStringLiteral("/work"));
        P.setStandardInputFile(QProcess::nullDevice());
        P.setStandardOutputFile(QProcess::nullDevice());
        P.setStandardErrorFile(QProcess::nullDevice());
        P.start(Builtin ? Bash : QStringLiteral("/bin/sh"), {QStringLiteral("-c"), Command});
        P.waitForFinished(5000);
#ifdef Q_OS_MACOS
        // there env has no long option, and bash 3.2 no "test -v", and no array element in "printf -v"
        bool Proves = !Command.contains(QStringLiteral("--split-string")) && !Command.contains(QStringLiteral("-v 'a["));
#else
        bool Proves = true;
#endif
        // the program really does that, or this test proves nothing
        CHECK_MSG(!Proves || QFile::exists(Canary), Command);
        QFile::remove(Canary);
      }
      Refused(D, Command);
    }
  }
  int Cases = qEnvironmentVariableIsSet("NIXSCP_RULES_FUZZ") ? qEnvironmentVariableIntValue("NIXSCP_RULES_FUZZ") : 1500;
  quint32 Seed = qEnvironmentVariableIsSet("NIXSCP_RULES_SEED") ? static_cast<quint32>(qEnvironmentVariableIntValue("NIXSCP_RULES_SEED")) : 20261105u;
  for (const char * Name : {"bash", "dash", "sh"})
  {
    QString Shell = QStandardPaths::findExecutable(QString::fromLatin1(Name));
    if (!Shell.isEmpty())
    {
      DifferentialFuzz(Shell, Dir.path(), Cases, Seed);
    }
  }
}

// What the JSON of a request makes of an escape: \ud800 stays in the QString
static QString JsonText(const QByteArray & Escaped)
{
  return QJsonDocument::fromJson("{\"c\":\"" + Escaped + "\"}").object().value(QStringLiteral("c")).toString();
}

// Runs what the server would get. true: the marker file appeared
static bool RunsInBash(const QString & Command, const QString & Marker)
{
  QString Bash = QStandardPaths::findExecutable(QStringLiteral("bash"));
  if (Bash.isEmpty())
  {
    return false;
  }
  QFile::remove(Marker);
  QProcess P;
  P.setStandardInputFile(QProcess::nullDevice());
  P.setStandardOutputFile(QProcess::nullDevice());
  P.setStandardErrorFile(QProcess::nullDevice());
  P.start(Bash, {QStringLiteral("-c"), QString::fromUtf8(Command.toUtf8())});
  P.waitForFinished(5000);
  bool Result = QFile::exists(Marker);
  QFile::remove(Marker);
  return Result;
}

// The rules checked one text and the server got another: toUtf8() drops a lone
// surrogate, and a regular expression does not match text that has one
static void TestLoneSurrogates(const QString & Folder)
{
  SessionData P = ReadOnly();
  SessionData DenyOnly;
  DenyOnly.AgentDeny = QStringLiteral("rm\nrm *\n/bin/rm *");
  SessionData NoRules;
  QString Marker = Folder + QStringLiteral("/substitution-ran");
  // the proof of the review: "$<surrogate>(" is no substitution to the parser,
  // and one for bash once the surrogate is gone
  for (const QByteArray & Escaped : {
    QByteArray(R"J(ls < \"$\ud800(touch )J") + Marker.toUtf8() + R"J()\")J",
    QByteArray(R"J(ls < \"$\udc00(touch )J") + Marker.toUtf8() + R"J()\")J",
    QByteArray(R"J(cat /etc/hostname < \"`\ud800touch )J") + Marker.toUtf8() + R"J(`\")J",
    QByteArray(R"J(echo $\udfff(touch )J") + Marker.toUtf8() + R"J())J",
    QByteArray(R"J(echo x\ud800; touch )J") + Marker.toUtf8()})
  {
    QString Command = JsonText(Escaped);
    CHECK(!Command.isEmpty());
    // the payload is alive: these bytes do run it. Only where toUtf8() drops the
    // surrogate: Qt before 6.10 makes "?" of it, which ends the substitution
    if (!QStandardPaths::findExecutable(QStringLiteral("bash")).isEmpty() && !Command.toUtf8().contains('?'))
    {
      CHECK_MSG(RunsInBash(Command, Marker), QString::fromLatin1(Escaped));
    }
    Refused(P, Command);
    Refused(DenyOnly, Command);
    Refused(NoRules, Command);
  }
  for (const QByteArray & Escaped : {
    QByteArray(R"J(/bin/r\ud800m x)J"), QByteArray(R"J(r\udc00m x)J"), QByteArray(R"J(\ud800rm x)J"), QByteArray(R"J(rm\ud800 x)J"),
    QByteArray(R"J(rm x\ud800)J"), QByteArray(R"J(rm \ud800\ud800x)J"), QByteArray(R"J(r\ud800Am)J"), QByteArray(R"J(r\udc00\ud800m)J")})
  {
    QString Command = JsonText(Escaped);
    Refused(DenyOnly, Command);
    Refused(P, Command);
    Refused(NoRules, Command);
  }
  Refused(P, JsonText(R"J(find . -\ud800delete)J"));
  Refused(P, JsonText(R"J(find . -\ud800exec rm x ;)J"));
  // an allowed pattern spelled exactly like it: the one way past the parser
  SessionData Exact;
  Exact.AgentAllow = QStringLiteral("uptime");
  Refused(Exact, JsonText(R"J(uptime\ud800)J"));
  // a real pair is plain text, and stays what it is
  Allowed(P, JsonText(R"J(echo \ud83d\ude00)J"));
  Allowed(NoRules, JsonText(R"J(echo \ud83d\ude00)J"));
  CHECK(JsonText(R"J(echo \ud83d\ude00)J").toUtf8() == "echo \xf0\x9f\x98\x80");
  // a byte order mark at the start does not survive the way back. As UTF-8 in the
  // JSON the parser takes it away itself, which changes nothing: it is not in the
  // text that is checked and sent. In the middle it stays, as a part of the word
  CHECK(JsonText("\xef\xbb\xbfrm x") == QStringLiteral("rm x"));
  for (const QString & Marked : {QStringLiteral("\ufeffrm x"), JsonText(R"J(\ufeffrm x)J"), JsonText(R"J(\ufeff\ufeffrm x)J")})
  {
    Refused(NoRules, Marked);
    Refused(DenyOnly, Marked);
    Refused(P, Marked);
  }
  Allowed(NoRules, JsonText(R"J(echo a\ufeffb)J"));
}

// Unicode that QString treats as equal or as white space, and the shell does not:
// rules must not read the line as something it is not
static void TestTextIsTakenAsItIs()
{
  SessionData D;
  D.AgentAllow = QStringLiteral("git status\necho a b\necho caf\u00e9\ncat *\nuptime");
  // to the shell a no-break space or any other is a part of the word
  for (const char16_t Space : {u'\u00a0', u'\u2003', u'\u3000', u'\u0085', u'\u2028'})
  {
    Refused(D, QStringLiteral("git") + QChar(Space) + QStringLiteral("status"));
    Refused(D, QStringLiteral("echo a") + QChar(Space) + QStringLiteral("b"));
    Refused(D, QStringLiteral("uptime") + QChar(Space));
    Refused(D, QChar(Space) + QStringLiteral("uptime"));
  }
  // the one way past the parser: only what the shell skips is skipped
  for (const char * End : {"\r", "\x0b", "\x0c"})
  {
    Refused(D, QStringLiteral("uptime") + QLatin1String(End));
    Refused(D, QLatin1String(End) + QStringLiteral("uptime"));
  }
  Allowed(D, QStringLiteral(" \t\nuptime \n\t"));
  // quotes keep a word together: it is not the two words of the pattern
  Refused(D, QStringLiteral("echo 'a  b'"));
  Refused(D, QStringLiteral("echo \"a\tb\""));
  Refused(D, QStringLiteral("echo 'a\nb'"));
  Allowed(D, QStringLiteral("echo a b"));
  // a program with a space in its name, and a path: not cat with an argument
  Refused(D, QStringLiteral("'cat /etc/passwd'"));
  Refused(D, QStringLiteral("\"cat ../x\" y"));
  Allowed(D, QStringLiteral("cat /etc/passwd"));
  // ...which an allowed pattern can still name, line for line
  SessionData Spaced;
  Spaced.AgentAllow = QStringLiteral("\"/opt/my app/run\" now");
  Allowed(Spaced, QStringLiteral("\"/opt/my app/run\" now"));
  // no normalization: another spelling is another text
  Allowed(D, QStringLiteral("echo caf\u00e9"));
  Refused(D, QStringLiteral("echo cafe\u0301"));
  // deny patterns see through more, not less
  SessionData Deny;
  Deny.AgentDeny = QStringLiteral("rm *");
  Refused(Deny, QStringLiteral("rm  -rf   x"));
  Refused(Deny, QStringLiteral("rm 'a  b'"));
  Refused(Deny, QStringLiteral("rm\u00a0x"));
}

// PCRE gives up on a long line under a pattern with several wildcards, as it
// does on invalid text: an error, which must not read as "does not match"
static void TestMatchErrors()
{
  SessionData P = ReadOnly();
  QString Dashes(5000, QLatin1Char('-'));
  Refused(P, QStringLiteral("file -C -m x ") + Dashes);
  Refused(P, QStringLiteral("ss -K dst 192.0.2.1 ") + Dashes);
  Refused(P, QStringLiteral("ss -D /tmp/x ") + QString(60000, QLatin1Char('-')));
  SessionData Deny;
  Deny.AgentDeny = QStringLiteral("rm *a*b*c*");
  Refused(Deny, QStringLiteral("rm abc ") + QString(30000, QLatin1Char('a')));
  // not a rule that breaks everything: nothing long about a short line
  Refused(Deny, QStringLiteral("rm abc"));
  Allowed(Deny, QStringLiteral("rm x"));
  // and long lines are fine under the rules they do not trouble
  Allowed(P, QStringLiteral("echo ") + QString(60000, QLatin1Char('x')));
  Allowed(P, QStringLiteral("file -b ") + QString(3000, QLatin1Char('x')));
  // an allowed pattern that fails to match is just not a match
  SessionData Allow;
  Allow.AgentAllow = QStringLiteral("x *a*b*c*d");
  Refused(Allow, QStringLiteral("x abc ") + QString(30000, QLatin1Char('a')));
  // the check is bounded: this used to take the main thread away for minutes
  QElapsedTimer Timer;
  Timer.start();
  QString Words = QStringLiteral("sudo");
  while (Words.size() < 65000)
  {
    Words += QStringLiteral(" a");
  }
  CommandAllowed(P, Words);
  std::printf("a wrapper with %lld words: checked in %lld ms\n", static_cast<long long>(Words.size() / 2), static_cast<long long>(Timer.elapsed()));
  CHECK(Timer.elapsed() < 10000);
}

// Deny-only sites: programs that start other programs from their options, and
// what a shell does to the first word
static void TestHiddenPrograms()
{
  SessionData D;
  D.AgentDeny = QStringLiteral("rm\nrm *");
  // the option and its value are one word to the checker: "-crm x"
  for (const char * Command : {"su -c'rm x'", "su -c'rm x' root", "su root -c'rm x'", "su --command='rm x'", "su -lc'rm x'", "su -c 'ls; rm x'",
    "su -c\"rm x\"", "su -", "su", "/bin/su -c'rm x'", "echo 'rm x' | su", "runuser -c 'ls; rm x'", "runuser -u x -- rm x", "sg users -c 'ls; rm x'",
    "script -c 'ls; rm x'", "script -qc'rm x' /dev/null", "watch 'ls; rm x'", "watch -n1 'ls; rm x'",
    "env -S'rm x'", "env -S 'rm x'", "env -vS'rm x'", "env -iS'rm x'", "env --split-string='rm x'", "env --split-string 'rm x'", "env --split='rm x'",
    "env --s='rm x'", "env -S'sh -c \"rm x\"'", "env -u X -S'rm x'", "env -uX -S'rm x'", "env -i -S'rm x'",
    "flock -c'rm x'", "flock f -c'rm x'", "flock f -c 'ls; rm x'", "flock -n f -c 'ls; rm x'", "flock -w 5 f -c 'ls; rm x'", "flock -w5 f -c 'ls; rm x'",
    "flock f --command='ls; rm x'", "flock f --command 'ls; rm x'", "flock --timeout 5 f -c 'ls; rm x'",
    "sudo -s", "sudo -i", "sudo -s 'ls; rm x'", "sudo -i 'ls; rm x'", "sudo -iu root", "sudo -u root -s", "sudo -uroot -s", "sudo --shell", "sudo --login",
    "sudo --user root --shell", "sudo --user=root -s", "doas -s", "doas -u root -s",
    "strace -o'|rm x' ls", "strace -o '|rm x' ls", "strace --output='|rm x' ls",
    // inside each other, as words or found by xargs and find
    "sudo su -c'rm x'", "nice env -S'rm x'", "xargs flock f -c'rm x'", "find . -exec su -c'rm x' ;", "sudo env -S'rm x'", "timeout 5 sudo -s", "busybox su -c'rm x'",
    // zsh: =rm is the path of rm, and - starts a program too
    "=rm x", "=/bin/rm x", "sudo =rm x", "xargs =rm", "find . -exec =rm {} +", "env =rm x", "- rm x", "ls &!rm x", "ls &! rm x",
    // file systems that ignore case
    "BASH -c 'rm x'", "Sh -c 'rm x'", "SUDO rm x", "Su -c'rm x'", "Find . -exec rm {} +",
    // more of the kind
    "/usr/bin/time rm x", "pkexec rm x", "setpriv rm x", "taskset 1 rm x", "chrt 1 rm x", "toybox rm x", "systemd-run rm x", "sudo -- rm x"})
  {
    Refused(D, QString::fromUtf8(Command));
  }
  // bash evaluates these, though they are quoted
  for (const char * Command : {"let 'x[$(rm y)]=1'", "declare 'a[$(rm y)]=1'", "typeset 'a[$(rm y)]=1'", "printf -v 'a[$(rm y)]' x", "read 'a[$(rm y)]'",
    "test -v 'a[$(rm y)]'", "let 'x[`rm y`]=1'", "command printf -v 'a[$(rm y)]' x", "builtin let 'x[$(rm y)]=1'", "printf -v 'a[${ rm y;}]' x",
    "compgen -C 'rm y'", "compgen -W '$(rm y)'", "complete -C 'rm y' x", "mapfile -C 'rm y' -c 1", "readarray -C 'rm y' -c 1", "bind -x 'a:rm y'", "fc -s", "enable -f x y"})
  {
    Refused(D, QString::fromUtf8(Command));
  }
  // also where the program is allowed, which was not meant for this
  SessionData Printf;
  Printf.AgentAllow = QStringLiteral("printf *\nlet *");
  Refused(Printf, QStringLiteral("printf -v 'a[$(rm y)]' x"), "subscript");
  Refused(Printf, QStringLiteral("let 'x[$(rm y)]=1'"), "subscript");
  Allowed(Printf, QStringLiteral("printf '%s' x"));
  Allowed(Printf, QStringLiteral("let 'x=1'"));
  // long lines cannot hide it: no regular expression in that check
  Refused(D, QStringLiteral("printf -v '") + QString(40000, QLatin1Char('[')) + QStringLiteral("$(rm y)]' x"));
  for (const char * Command : {"printf '%s' x", "test -f x", "read x", "let 'x=1'", "declare -p", "grep '[$(]' f", "echo '[$(x)]'"})
  {
    Allowed(D, QString::fromUtf8(Command));
  }
  // what names no shell and no denied program stays allowed
  for (const char * Command : {"sudo du -sh /var", "sudo ls -lash /var", "sudo -u root ls -la", "sudo -uroot ls -si", "sudo -n ls", "sudo -E ls", "sudo grep -E 'a|b' f",
    "sudo --user=root ls", "env -i ls", "env FOO=1 ls -S", "env -u X ls", "flock f ls", "flock -n f ls -c", "flock -w 5 f ls", "nice -n5 ls", "timeout -k5 10 ls",
    "xargs -I'{}' ls '{}'", "find . -exec ls -s {} +", "strace ls", "grep =foo f", "echo =rm", "echo '=rm x'", "ls - x"})
  {
    Allowed(D, QString::fromUtf8(Command));
  }
  // the user can allow them by name, as the shells
  SessionData Named;
  Named.AgentAllow = QStringLiteral("su -c *\nenv -S *");
  Allowed(Named, QStringLiteral("su -c 'ls'"));
  Allowed(Named, QStringLiteral("env -S ls"));
}

// X-M13: nothing in a name or a command starts another line or field
static void TestAuditLog(const QString & Folder)
{
  QFile::remove(AuditLogFileName());
  CHECK(AppendAudit(QStringLiteral("x\n2026-11-05T10:00:00\tprod\texec\tallowed\trm -rf /"), QStringLiteral("exec"), QStringLiteral("ok\rfake"),
    QStringLiteral("denied: not allowed - Site \"x\nfake\" is not available")));
  QFile Log(AuditLogFileName());
  CHECK(Log.open(QIODevice::ReadOnly));
  QByteArray Text = Log.readAll();
  Log.close();
  CHECK_MSG(Text.count('\n') == 1, QString::fromUtf8(Text));
  CHECK_MSG(Text.count('\t') == 4, QString::fromUtf8(Text));
  CHECK(!Text.contains('\r'));
  CHECK(Text.contains("x\\n2026-11-05T10:00:00\\tprod"));
  CHECK((Log.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther)) == 0);
  // a file that was too open gets closed down
  CHECK(Log.setPermissions(Log.permissions() | QFileDevice::ReadOther));
  CHECK(AppendAudit(QStringLiteral("s"), QStringLiteral("exec"), QString(600, QLatin1Char('a')), QStringLiteral("allowed")));
  CHECK((Log.permissions() & QFileDevice::ReadOther) == 0);
  CHECK(Log.open(QIODevice::ReadOnly));
  Text = Log.readAll();
  Log.close();
  CHECK(Text.contains(QByteArray(500, 'a') + "...\n") && !Text.contains(QByteArray(501, 'a')));
  // L7: when it cannot be written the caller is told
  CHECK(QFile::remove(AuditLogFileName()));
  CHECK(QDir(Folder).mkdir(QFileInfo(AuditLogFileName()).fileName()));
  CHECK(!AppendAudit(QStringLiteral("s"), QStringLiteral("exec"), QStringLiteral("x"), QStringLiteral("allowed")));
  CHECK(QDir(Folder).rmdir(QFileInfo(AuditLogFileName()).fileName()));

  CHECK(VisibleText(QStringLiteral("a\nb\tc\rd"), false) == QStringLiteral("a\\nb\\tc\\rd"));
  CHECK(VisibleText(QStringLiteral("a\nb\tc\rd"), true) == QStringLiteral("a\nb\tc\\rd"));
  CHECK(VisibleText(QString::fromUtf8("a\xe2\x80\xae" "b\x1b" "c"), true) == QStringLiteral("a\\u202eb\\x1bc"));
  CHECK(VisibleText(QString::fromUtf8("\xc5\xa1" "ef"), false) == QString::fromUtf8("\xc5\xa1" "ef"));
  // what cannot be seen: format characters, and what toUtf8() would drop
  CHECK(VisibleText(QStringLiteral("r\u200bm\ufeff\u00ad\u2060"), false) == QStringLiteral("r\\u200bm\\ufeff\\xad\\u2060"));
  CHECK(VisibleText(QStringLiteral("a\U000E0041b"), false) == QStringLiteral("a\\U000e0041b"));
  CHECK(VisibleText(QStringLiteral("a") + QChar(0xD800) + QStringLiteral("b") + QChar(0xDC00), false) == QStringLiteral("a\\ud800b\\udc00"));
  CHECK(VisibleText(QStringLiteral("\U0001F600 \u00e9"), false) == QStringLiteral("\U0001F600 \u00e9"));
  // the check for all text of a request
  CHECK(ExactUtf8(QString()) && ExactUtf8(QStringLiteral("plain")) && ExactUtf8(QStringLiteral("\U0001F600 \u00e9 a\ufeffb \uffff")));
  CHECK(!ExactUtf8(QStringLiteral("a") + QChar(0xD800)) && !ExactUtf8(QChar(0xDC00) + QStringLiteral("a")) && !ExactUtf8(QStringLiteral("\ufeffa")));
}

// S27, L8
static void TestRecordings()
{
  QDateTime Now = QDateTime::currentDateTimeUtc();
  QDir Recordings(RecordingDirectory());
  // no NixSCP key: nothing lands on disk, in any form
  ConfigVault::Forget();
  CHECK(!CanRecord());
  CHECK(!SaveRecording(Now, QStringLiteral("site"), QStringLiteral("exec"), QStringLiteral("cat secret"), "top secret output", false, 0, 0, QString()));
  CHECK(Recordings.entryList({QStringLiteral("*.log")}, QDir::Files).isEmpty());

  QByteArray Dek(CloudCrypto::KeyLength, 'k');
  CloudCrypto::SnapshotHeader Wrap;
  Wrap.User = QStringLiteral("agent@nixscp.test");
  Wrap.DekPassword = QByteArray(48, 'w');
  ConfigVault::SetKey(Dek, Wrap);
  CHECK(ConfigVault::Enabled() && CanRecord());
  // same millisecond, same site, same request: a file each
  for (int I = 0; I < 3; I++)
  {
    CHECK(SaveRecording(Now, QStringLiteral("site"), QStringLiteral("exec"), QStringLiteral("cmd %1").arg(I), QByteArray("output ") + QByteArray::number(I), false, 0, I, QString()));
  }
  QStringList Files = Recordings.entryList({QStringLiteral("*.log")}, QDir::Files);
  CHECK_MSG(Files.size() == 3, Files.join(QLatin1Char(' ')));
  QSet<QString> Seen;
  for (const QString & Name : Files)
  {
    QFile Raw(Recordings.filePath(Name));
    CHECK(Raw.open(QIODevice::ReadOnly));
    QByteArray Bytes = Raw.readAll();
    CHECK(ConfigVault::IsSealed(Bytes) && !Bytes.contains("output"));
    CHECK((Raw.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)) == 0);
    QByteArray Text = ConfigVault::ReadFile(Recordings.filePath(Name));
    CHECK(Text.contains("output "));
    Seen.insert(QString::fromUtf8(Text.mid(Text.indexOf("Command: "), 14)));
  }
  CHECK(Seen.size() == 3);
  ConfigVault::Forget();
}

// L13: a socket in a folder other users can write to could be taken over
static void TestSocketName(const QString & Folder)
{
#ifndef Q_OS_WIN
  QByteArray Saved = qgetenv("XDG_RUNTIME_DIR");
  QString Private = Folder + QStringLiteral("/private");
  QString Shared = Folder + QStringLiteral("/shared");
  CHECK(QDir().mkpath(Private) && QDir().mkpath(Shared));
  QFile::setPermissions(Private, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  QFile::setPermissions(Shared, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
    QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther);
  qputenv("XDG_RUNTIME_DIR", Private.toUtf8());
  CHECK(SocketName() == Private + QStringLiteral("/nixscp-agent.sock"));
  // not private, missing, unset: never a temporary folder others can write to
  QString Fallback = QDir::homePath() + QStringLiteral("/.nixscp-agent.sock");
#ifdef Q_OS_MACOS
  // the one of macOS is the user's own; one of the others is not used either
  QByteArray SavedTemp = qgetenv("TMPDIR");
  qunsetenv("XDG_RUNTIME_DIR");
  qputenv("TMPDIR", Shared.toUtf8());
  CHECK_MSG(SocketName() == Fallback, SocketName());
  qputenv("TMPDIR", Private.toUtf8());
  Fallback = Private + QStringLiteral("/nixscp-agent.sock");
  CHECK_MSG(SocketName() == Fallback, SocketName());
#endif
  qputenv("XDG_RUNTIME_DIR", Shared.toUtf8());
  CHECK_MSG(SocketName() == Fallback, SocketName());
  qputenv("XDG_RUNTIME_DIR", "/tmp");
  CHECK_MSG(SocketName() == Fallback, SocketName());
  qputenv("XDG_RUNTIME_DIR", (Folder + QStringLiteral("/missing")).toUtf8());
  CHECK_MSG(SocketName() == Fallback, SocketName());
  qunsetenv("XDG_RUNTIME_DIR");
  CHECK_MSG(SocketName() == Fallback, SocketName());
  qputenv("XDG_RUNTIME_DIR", Saved);
#ifdef Q_OS_MACOS
  qputenv("TMPDIR", SavedTemp);
#endif
#else
  Q_UNUSED(Folder);
#endif
}

// X-H4, X-M15
static void TestDownloadFile(const QString & Folder)
{
  QString Name = Folder + QStringLiteral("/existing.conf");
  auto Write = [&Name](const QByteArray & Data)
  {
    QFile F(Name);
    CHECK(F.open(QIODevice::WriteOnly) && (F.write(Data) == Data.size()));
  };
  auto Read = [&Name]()
  {
    QFile F(Name);
    return F.open(QIODevice::ReadOnly) ? F.readAll() : QByteArray("(missing)");
  };
  Write("precious\n");
  {
    // opened, written to, never committed: as a refused or failed download
    DownloadFile File(Name);
    CHECK(File.Open());
    CHECK(File.Write("half a downlo"));
    CHECK(Read() == "precious\n");
    File.Cancel();
    CHECK(Read() == "precious\n");
  }
  {
    DownloadFile File(Name);
    CHECK(File.Open() && File.Write("partial"));
    // goes out of scope: a client that died
  }
  CHECK(Read() == "precious\n");
  CHECK(QDir(Folder).entryList(QDir::Files).filter(QStringLiteral("existing.conf")).size() == 1);
  {
    DownloadFile File(Name);
    CHECK(File.Open() && File.Write("new ") && File.Write("content\n"));
    CHECK(Read() == "precious\n");
    CHECK(File.Commit());
    CHECK(Read() == "new content\n");
  }
  // the permissions of the file stay
  QFile::setPermissions(Name, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  {
    DownloadFile File(Name);
    CHECK(File.Open() && File.Write("x") && File.Commit());
  }
  CHECK((QFile(Name).permissions() & (QFileDevice::ReadOther | QFileDevice::ReadGroup)) == 0);
  // a new file, and one in a folder that does not exist
  {
    DownloadFile File(Folder + QStringLiteral("/new.bin"));
    CHECK(File.Open() && File.Write("abc") && File.Commit());
    CHECK(QFileInfo(Folder + QStringLiteral("/new.bin")).size() == 3);
    DownloadFile Nowhere(Folder + QStringLiteral("/missing/new.bin"));
    CHECK(!Nowhere.Open() && !Nowhere.ErrorString().isEmpty());
    CHECK(!Nowhere.Commit());
  }
#ifndef Q_OS_WIN
  // devices are written directly
  {
    DownloadFile File(QStringLiteral("/dev/null"));
    CHECK(File.Open() && File.Write("abc") && File.Commit());
  }
#endif
}

static void TestFrames()
{
  FrameReader Reader;
  Reader.Append(Frame(frStdin, "ab"));
  Reader.Append(Frame(frStdinEof).left(3));
  quint8 Type = 0;
  QByteArray Payload;
  bool Error = false;
  CHECK(Reader.Next(Type, Payload, Error) && (Type == frStdin) && (Payload == "ab"));
  CHECK(!Reader.Next(Type, Payload, Error) && !Error);
  FrameReader Big;
  Big.Append(Frame(frStdin, QByteArray(MaxFrameSize + 1, 'x')));
  CHECK(!Big.Next(Type, Payload, Error) && Error);
}

int main(int argc, char ** argv)
{
  QCoreApplication App(argc, argv);
  QTemporaryDir Dir;
  qputenv("HOME", Dir.path().toUtf8());
  Configuration::Instance().SetIniFileName(Dir.path() + QStringLiteral("/nixscp.ini"));
  TestAuditBypasses();
  TestFailClosed();
  TestDenyRules();
  TestAllowRules();
  TestReadOnlyPreset();
  TestLoneSurrogates(Dir.path());
  TestTextIsTakenAsItIs();
  TestMatchErrors();
  TestHiddenPrograms();
  TestAuditLog(Dir.path());
  TestRecordings();
  TestSocketName(Dir.path());
  TestDownloadFile(Dir.path());
  TestFrames();
  TestDifferential();
  if (Failures == 0)
  {
    std::printf("All agent rule tests passed\n");
  }
  return (Failures == 0) ? 0 : 1;
}
