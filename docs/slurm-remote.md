# Remote visualization on a Slurm cluster

Run the AMReXplorer desktop application locally and its server on a compute
node allocated by Slurm. A small launcher on the cluster discovers a named
allocation and starts the server with `srun`:

```text
Local AMReXplorer → SSH → cluster login node → srun → compute-node server
```

The connection uses stdin/stdout throughout. No VNC, X11, listening port, or
SSH port forwarding is needed. The allocation's job ID can change between
sessions without editing the launcher. This workflow uses AMReXplorer's
existing remote mode; it does not require scheduler support in the client.

## Prerequisites

- A local AMReXplorer client with SSH remote support (macOS or Linux).
- SSH access to a cluster login node where `squeue` and `srun` are available
  in noninteractive sessions, and site policy permits launching job steps
  from that node.
- A compatible `amrexplorer-server` installed on a filesystem visible to
  compute nodes. Use the same source revision as the client; see the
  [server-only build instructions](../INSTALL.md#hpc-systems). The examples
  assume `~/.local/bin/amrexplorer-server`.
- Plotfiles on a filesystem visible to the compute node.

Replace `USERNAME`, `PROJECT`, `LOGIN_HOST`, and dataset paths below with your
cluster's values. Partition names, memory limits, module setup, and allocation
policies vary by site; consult its documentation.

## 1. Reserve a named allocation

In a terminal, connect to the cluster and request one task with eight CPUs:

```bash
ssh USERNAME@LOGIN_HOST
salloc -A PROJECT -p PARTITION -N 1 -n 1 -c 8 \
    -t 01:00:00 --job-name=amrexplorer
```

Wait until the allocation is granted. Keep this terminal and its allocation
shell open while visualizing. Reserve this allocation for the server; another
job step consuming its CPUs can prevent the server step from starting. Add a
memory request appropriate to your data and the cluster's policy if needed.

The current client launches the server with eight worker threads, which is
why both allocation and launcher examples request eight CPUs. One server
process is sufficient for this workflow; it does not require a GPU allocation.

Use exactly one running allocation named `amrexplorer` per user on the
cluster. The launcher below rejects zero or multiple matches. For separate
workflows, use distinct job names and a corresponding launcher for each.

### Example: OLCF Riker

Riker provides CPU resources in the `batch` partition and supports partial-node
allocations, with cores and memory coupled by site policy. For example:

```bash
ssh USERNAME@riker-login1.olcf.ornl.gov
salloc -A PROJECT -p batch -N 1 -n 1 -c 8 \
    -t 01:00:00 --job-name=amrexplorer
```

A dataset path might be
`/lustre/orion/PROJECT/scratch/USERNAME/run/plt00010`. Check the current
[Riker user guide](https://docs.olcf.ornl.gov/systems/riker_user_guide.html)
for access, resource requests, and filesystem details.

## 2. Install the launcher once

On the cluster, create the script directory:

```bash
mkdir -p ~/bin
```

Save the following as `~/bin/amrexplorer-slurm`:

```bash
#!/bin/bash
set -euo pipefail

# Load any required Slurm/server runtime modules here.
# Send their output to stderr, for example: module load gcc >&2

# A separate SSH session does not inherit the allocation shell's job ID.
# Query Slurm instead. Capture separately so an squeue failure is fatal.
job_ids=$(squeue --local --noheader \
    --user="$(id -un)" \
    --name=amrexplorer \
    --states=RUNNING \
    --format='%A')

jobs=()
while read -r job_id; do
    [[ -z "$job_id" ]] || jobs+=("$job_id")
done <<< "$job_ids"

case ${#jobs[@]} in
    0)
        echo "No running 'amrexplorer' allocation. Run salloc first." >&2
        exit 1
        ;;
    1)
        ;;
    *)
        echo "Multiple running 'amrexplorer' allocations; use a unique job name." >&2
        exit 1
        ;;
esac

exec srun --jobid="${jobs[0]}" \
    --nodes=1 --ntasks=1 --cpus-per-task=8 \
    "$HOME/.local/bin/amrexplorer-server" "$@"
```

Make it executable:

```bash
chmod +x ~/bin/amrexplorer-slurm
```

Keep `"$@"`: the client passes `--stdio --threads 8` through the launcher to
the server. The lookup uses only your running jobs on the local Slurm cluster;
it does not select pending jobs or allocations from other federated clusters.
See the [Slurm squeue documentation](https://slurm.schedmd.com/squeue.html).

Keep stdout dedicated to the server protocol. Write launcher diagnostics and
module output to stderr. Do not add `srun --pty`, `--unbuffered`, `--label`,
`--async`, output-file redirection, or `2>&1`: these can alter or disconnect the
protocol stream. In particular, Slurm's `--unbuffered` uses a pseudo-terminal.
See the [Slurm srun documentation](https://slurm.schedmd.com/srun.html).

## Optional: size caches from the job's memory allowance

With a server that supports `--cache-memory-fraction`, add the option to the
launcher's final command:

```bash
exec srun --jobid="${jobs[0]}" \
    --nodes=1 --ntasks=1 --cpus-per-task=8 \
    "$HOME/.local/bin/amrexplorer-server" \
    --cache-memory-fraction 0.5 "$@"
```

Detection happens inside the compute-node server, after `srun` places it in
the job step. On Linux the server reads its cgroup v2 `memory.max` (or v1
`memory.limit_in_bytes`), including visible ancestor limits. It also considers
`SLURM_MEM_PER_NODE`, or `SLURM_MEM_PER_CPU` multiplied by
`SLURM_CPUS_ON_NODE`, when available. The smallest discovered bound wins,
limited by physical RAM. Unlimited cgroup values do not count as a bound.
See the [Linux cgroup documentation](https://docs.kernel.org/admin-guide/cgroup-v2.html)
and [Slurm environment variables](https://slurm.schedmd.com/sbatch.html).

The fraction must be greater than zero and at most one. `0.5` assigns half
of that limit to **one shared allowance for cached block data and sampled
volume grids across every dataset and connection in this server process**.
A dataset can use available space without dividing the allowance by the
maximum number of datasets. Under pressure, caches evict unpinned entries;
entries still used by active requests remain charged until released. If
space cannot be reclaimed, block queries report cache pressure and volume
grids can render uncached, as with the existing per-dataset cache limits.

The detected limit and shared allowance are logged to stderr during startup.
Detection is performed once; it does not track changes to the allocation or
measure currently free memory. For example, a detected 128 GiB limit with
`0.5` gives a 64 GiB shared cache allowance. This replaces the client's initial
block-cache request (normally 1 GiB). Later client budget changes can reduce
that dataset's budget, but cannot raise it above the shared allowance.
`--volume-cache-mib` remains an additional per-dataset volume-grid cap.

Leave headroom: the allowance covers cached payloads, **not total process
memory**. Metadata, particles, in-flight block reads, uncached volume grids,
rendering buffers, and other job processes consume additional memory. Half
is a starting policy, not a guarantee against an out-of-memory failure.
Separate server processes have separate allowances; use one server process
per allocation with this recipe.

If detection fails, the server refuses startup with an explanation. In a
Slurm job it will not substitute the whole node's RAM for an unknown job
limit; an explicit `SLURM_MEM_PER_NODE=0` represents Slurm's all-node-memory
request. Outside Slurm, Linux physical RAM is the fallback when no finite
cgroup limit is found. On other operating systems, automatic detection needs
the Slurm memory variables. Omit the option to retain existing client-selected
cache budgets; setting `AMREXPLORER_CACHE_SIZE_MB` in the remote launcher does
not configure the block cache because that variable is read by the local
client.

## 3. Connect from the local client

```bash
amrexplorer --ssh USERNAME@LOGIN_HOST \
    --server '~/bin/amrexplorer-slurm' \
    /shared/path/to/plt00010
```

For the Riker example:

```bash
amrexplorer --ssh USERNAME@riker-login1.olcf.ornl.gov \
    --server '~/bin/amrexplorer-slurm' \
    /lustre/orion/PROJECT/scratch/USERNAME/run/plt00010
```

Alternatively, choose **File > Open Remote Plotfile...** or **File > Open
Remote Plotfile Sequence...**. Enter the login-node SSH destination and
`~/bin/amrexplorer-slurm` as the server executable, then use **Browse...** or
enter the remote path. The client remembers the executable per destination.
SSH aliases and authentication work as described in the
[remote-datasets guide](user-guide.md#remote-datasets).

Each new connection discovers the current allocation and starts a server job
step inside it. You do not need to know the compute-node hostname or start a
server manually. Use one active server session per allocation with this
eight-CPU setup; additional sessions may wait for resources.

## 4. Finish and release resources

Close the remote viewer to end its server session. The separately reserved
Slurm allocation remains active until you release it or its walltime expires.
Exit the allocation shell in the original terminal, or cancel the allocation
explicitly using its ID:

```bash
squeue --local --user="$(id -un)" --name=amrexplorer
scancel JOB_ID
```

For the next session, repeat the named `salloc` request and reconnect. The
launcher requires no changes.

## Troubleshooting

- **No running allocation:** wait for `salloc` to grant resources. Check the
  job name, owner, and cluster. Pending jobs are intentionally excluded.
- **Multiple matching allocations:** release the unused allocation or give
  each workflow a distinct name and update its launcher's `--name` filter.
- **Connection startup times out:** the client currently allows five minutes
  for startup, including authentication and server launch. Allocate before
  connecting and ensure another step is not occupying the reserved CPUs.
- **Missing command or shared library:** noninteractive SSH may not load the
  same environment as your terminal. Initialize the site's module system and
  load required modules in the launcher, with output directed to stderr;
  verify the server path is accessible from compute nodes.
- **Allocation ends during use:** walltime expiry or cancellation terminates
  the server. Obtain a new allocation and reconnect.

An automatic `srun` resource request inside the launcher is possible, but its
queue wait would count against the client's startup timeout. This workflow
keeps allocation separate so queue delays do not interrupt connection setup.
