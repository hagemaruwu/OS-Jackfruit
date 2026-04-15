# OS-Jackfruit: Multi-Container Runtime & Kernel Monitor

**Team Member:** Aditya Raj (and Team)

This project implements a lightweight Linux container runtime consisting of a user-space supervisor (`engine.c`) and a kernel-space memory monitor (`monitor.c`). It provides process isolation, concurrent logging, and strict resource enforcement.

## 🚀 Getting Started

### 1. Build & Load
```bash
# Compile both user-space binaries and the kernel module
cd boilerplate
make

# Load the memory monitor module
sudo insmod monitor.ko
```

### 2. Start Supervisor
```bash
sudo ./engine supervisor ./rootfs-base
```

### 3. Run Containers
```bash
# In a separate terminal
sudo ./engine start c1 ./rootfs-alpha "/bin/sh -c './cpu_hog'" --hard-mib 128
sudo ./engine ps
```

---

## 🏗️ Architecture & Design Decisions

### 1. Control Plane IPC (UNIX Domain Sockets)
We chose **UNIX Domain Sockets** for the CLI-to-Supervisor communication.
- **Tradeoff:** It is more complex than FIFOs but supports full bidirectional communication and structured message passing (`control_request_t`).
- **Justification:** Essential for the `ps` and `logs` commands where the supervisor must send data back to the client reliably.

### 2. Isolation (Namespaces & Chroot)
The runtime uses the `clone()` system call with `CLONE_NEWPID`, `CLONE_NEWUTS`, and `CLONE_NEWNS`.
- **Logic:** This ensures the container sees its own PID 1 and has its own isolated hostname.
- **Filesystem:** We used `chroot` for ease of implementation, providing a clean jail within the provided `rootfs-*` directory.

### 3. Bounded-Buffer Logging
A concurrent pipeline handles container output:
- **Synchronization:** Uses a `pthread_mutex_t` and two `pthread_cond_t` (not_full, not_empty).
- **Benefit:** This prevents the supervisor from dropping log lines even if the disk I/O is slow, acting as an elastic buffer.

### 4. Kernel Memory Monitor
The LKM tracks processes in a linked list and monitors RSS (Resident Set Size).
- **Soft Limit:** Triggers an alert in `dmesg` to warn the supervisor.
- **Hard Limit:** Sends an immediate `SIGKILL` to prevent a rogue container from crashing the host.

---

## 📈 Engineering Analysis

### 1. Isolation Mechanism
Namespaces allow the host to share its kernel with containers while providing the *illusion* of a private system. The host kernel still manages all hardware, but the container's view of PIDs and mount points is strictly restricted.

### 2. Process Lifecycle
The long-running supervisor is the "reaper." By handling `SIGCHLD`, it ensures that even if a container crashes, its resources are freed and it is properly removed from the process table, preventing "zombie" accumulation.

### 3. Memory & RSS
RSS measures the actual physical RAM occupied by a process. We enforce limits in kernel-space because it is the only way to guarantee a process cannot ignore the limit—a user-space monitor might be slow or preempted, allowing a memory spike to go unchecked.

---

## 🧪 Scheduler Experiments
Using the `--nice` flag, we demonstrated that the Linux Completely Fair Scheduler (CFS) correctly prioritizes containers with lower nice values during CPU contention, granting them a larger share of the CPU cycles as measured by completion time of the `cpu_hog` workload.
