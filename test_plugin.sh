#/bin/bash
set -euo pipefail

# defaults
TOTAL_NODES=2
PLUGIN_PATH="/g/g92/marathe1/myworkspace/fractale/flux-multi-cluster-utilities/install/lib/flux/job-manager/plugins/delegate.so"

CHILD_NODES=$(( TOTAL_NODES / 2 ))

#flux alloc -N"$TOTAL_NODES"

flux resource list

jobA=$(flux submit -N"$CHILD_NODES" flux start sleep inf | tail -n1)
jobB=$(flux submit -N"$CHILD_NODES" flux start sleep inf | tail -n1)

local_uri=$(flux proxy "$jobB" flux getattr local-uri | tr -d '[:space:]')
remote_uri=$(flux uri --remote "$local_uri")
flux proxy "$jobA"

flux resource list

flux jobtap load "$PLUGIN_PATH"

