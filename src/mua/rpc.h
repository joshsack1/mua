#ifndef MUA_RPC_H
#define MUA_RPC_H

// The --embed msgpack-RPC server: reads requests on stdin and writes responses
// on stdout, dispatching through the shared api_dispatch_table (Lua-free). Runs
// the event loop until stdin closes (or a SIGINT stops it). Returns a process
// exit code (0 ok, 1 on a transport setup failure). A read/write API server --
// it does not run agent turns, so no session exists and no autocmd events fire.
int rpc_serve(void);

#endif // MUA_RPC_H
