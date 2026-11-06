// NixSCP Agent Access broker (core/AgentAccess.h): a local socket server in
// the running application. Requests from "nixscp --connect/--get/--put/--list"
// are checked (encrypted configuration, NixSCP Pro license, per-site agent access,
// permissions, approval) and executed over SSH by a worker thread, with the
// credentials of the stored site, which never leave this process.
#pragma once

#include <QObject>

class QWidget;

namespace AgentBroker
{
  // Starts listening (once per process); Window parents the approval prompts
  void Start(QWidget * Window);
  // Number of requests running now, tunnels included (tests, diagnostics)
  int ActiveJobs();
  // Why agents cannot reach this instance, when it is not just that another
  // NixSCP serves them; empty when all is well
  QString ListenError();
}
