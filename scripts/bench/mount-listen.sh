#!/usr/bin/env bash
# mount-listen.sh <cli> <port> : start a wsldrive mount that WAITS for the agent
# to dial in (client listens). Used for Direction B so a Windows agent can reach
# the WSL client via localhost forwarding without a WSL->host firewall opening.
cli="$1"; port="$2"
mkdir -p /tmp/benchB
nohup "$cli" mount /tmp/benchB --listen "tcp://0.0.0.0:$port" >/tmp/benchB.log 2>&1 &
echo started
