// "nixscp --mcp": Agent Access as a Model Context Protocol server (JSON-RPC
// 2.0, one message per line on standard input/output). The tools use the
// running NixSCP application exactly like "nixscp --connect/--get/--put/--list"
// (core/AgentAccess.h): same rules, approvals, recordings and audit log.
#pragma once

namespace AgentMcp
{
  // Serves until standard input ends; needs a QCoreApplication
  int Serve();
}
