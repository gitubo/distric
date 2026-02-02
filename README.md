# DistriC 2.0

**DistriC** is a distributed coordination and task execution platform built for systems that need **strong consistency where it matters**, **fast failure detection**, and **horizontal scalability**.

At its core, DistriC combines two proven ideas:
- **Raft** for reliable coordination and configuration
- **Gossip-based protocols** for scalable membership and health propagation

The result is a system that stays consistent, reacts quickly to failures, and scales cleanly as the cluster grows.

---

## What DistriC Is About

DistriC is designed for environments where:
- Configuration and control must be consistent and auditable
- Work needs to be distributed across many nodes
- Nodes may come and go dynamically
- Failures are expected and must be handled gracefully
- Operational visibility is not optional

Rather than relying on a single coordination mechanism, DistriC deliberately splits responsibilities between **consensus** and **gossip**, using each where it fits best.

---

## Architecture at a Glance

DistriC runs as a cluster composed of two logical roles:

**Coordination nodes**
- Maintain authoritative system state
- Elect a leader using Raft
- Replicate configuration and control data
- Decide where work should run

**Worker nodes**
- Execute assigned tasks
- Report health, load, and execution status
- Participate in gossip-based failure detection
- Scale horizontally as needed

This separation allows the system to remain strongly consistent without sacrificing scalability.

---

## Key Features

### Strong Coordination Without Central Bottlenecks
DistriC uses Raft to ensure that critical state changes are serialized, replicated, and agreed upon. This makes leader election, configuration updates, and cluster control predictable and safe.

### Fast and Scalable Failure Detection
Instead of routing all health information through a central point, DistriC relies on gossip-style communication. Nodes exchange health and membership information continuously, allowing failures to be detected and propagated quickly even in large clusters.

### Protocol-First Design
All communication is defined by an explicit, versioned binary protocol. Message formats, payloads, and evolution rules are clearly specified, making the system easier to reason about, debug, and extend over time.

### Configuration as Data
Rules, workflows, and system behavior are treated as data rather than code. This allows changes to be versioned, replicated, audited, and rolled back without redeploying binaries.

### Built for Observability
From the start, DistriC is designed to expose what it is doing. Metrics, structured logs, tracing, and health information are considered part of the system contract, not optional add-ons.

### Modular and Layered
Each part of the system has a single responsibility. Networking, protocol definition, coordination logic, and execution are kept separate, making the codebase easier to evolve and reason about.

---

## Design Principles

- Use the right coordination mechanism for the job
- Prefer explicit protocols over implicit behavior
- Make failure a normal, expected condition
- Optimize for clarity and evolvability
- Keep operational insight built in, not bolted on

---

## Who This Is For

DistriC is aimed at:
- Distributed systems engineers
- Infrastructure and platform teams
- Developers building coordination-heavy or workflow-driven systems
- Anyone interested in hybrid consensus and gossip architectures

---

## License

MIT License
