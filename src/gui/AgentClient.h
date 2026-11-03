// "nixscp --list | --connect | --get | --put | --help": the Agent Access client
// (core/AgentAccess.h). Talks to the running NixSCP application; never sees
// passwords or keys.
#pragma once

#include <QStringList>

namespace AgentClient
{
  // Runs the command when Arguments (without the program name) start with
  // one of the agent switches; -1 otherwise (normal NixSCP start)
  int Run(int argc, char * argv[], const QStringList & Arguments);
}
