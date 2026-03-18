<!--
  Sync Impact Report
  Version change: 0.0.0 (template) → 1.0.0
  Modified principles: All new (template placeholders → concrete principles)
  Added sections:
    - 5 Core Principles (Requirements-Driven, Architecture-First, Quality-First,
      Test-Driven Verification, Coordinated Delivery)
    - Agent Team Collaboration (roles and responsibilities)
    - Development Workflow (phase gates and review process)
  Removed sections: None (first fill)
  Templates requiring updates:
    - .specify/templates/plan-template.md: ✅ No changes needed (Constitution Check
      section already references constitution generically)
    - .specify/templates/spec-template.md: ✅ No changes needed (spec sections align
      with Requirements-Driven principle)
    - .specify/templates/tasks-template.md: ✅ No changes needed (phase structure
      and parallel execution align with Coordinated Delivery principle)
  Follow-up TODOs: None
-->

# AXON Transport Library Constitution

## Core Principles

### I. Requirements-Driven Development

Every feature MUST start from a validated specification that defines user value,
measurable success criteria, and testable acceptance scenarios. The Product
Manager (PM) role owns requirement completeness:

- All features MUST have prioritized user stories (P1/P2/P3) before design begins
- Each user story MUST be independently testable and deliverable
- Requirements MUST be technology-agnostic — no implementation details in specs
- Success criteria MUST include quantitative metrics (latency, throughput, coverage)
- Maximum 3 NEEDS CLARIFICATION markers per spec; resolve before planning

### II. Architecture-First Design

System architecture MUST be designed and reviewed before implementation begins.
The Technical Architect (Arch) role owns structural integrity:

- All technical decisions MUST be documented with rationale and alternatives considered
- The architecture MUST enforce separation of concerns (layered design)
- Public API surface MUST be minimal and stable — internal details hidden
- Performance-critical paths MUST be identified and designed for zero-overhead abstraction
- Dependencies MUST be explicitly justified; no unnecessary external libraries
- Thread safety model MUST be defined and enforced (thread-per-worker for this project)

### III. Quality-First Engineering

Code quality and correctness are non-negotiable. The Development Engineer (Dev)
role owns implementation quality:

- All code MUST compile with zero warnings under `-Wall -Wextra -Werror`
- Public APIs MUST have complete header documentation
- Error handling MUST use the project's Status type — no raw exceptions on public API boundaries
- Memory management MUST follow RAII; no manual new/delete in application code
- Code MUST pass static analysis (clang-tidy) and formatting (clang-format) checks
- Each commit MUST build and pass existing tests

### IV. Test-Driven Verification

Testing is mandatory at every level. The QA Engineer (QA) role owns verification
completeness:

- Unit tests MUST be written for all core modules (target: 80% line coverage)
- Integration tests MUST cover all acceptance scenarios from the specification
- Performance benchmarks MUST validate success criteria (SC-001 through SC-003)
- Fault injection tests MUST verify error paths (timeout, peer crash, truncation)
- Tests MUST run on CI for every PR; no merges with failing tests
- Red-Green-Refactor cycle: write failing test → implement → verify pass → refactor

### V. Coordinated Delivery

Work MUST be planned, tracked, and delivered incrementally. The Project
Coordinator (Coord) role owns process and communication:

- Work MUST be organized by user story priority (P1 first, then P2, then P3)
- Each phase MUST have a clear checkpoint with acceptance criteria
- Parallel work MUST be identified and assigned to avoid conflicts
- All decisions, risks, and trade-offs MUST be documented in meeting minutes
- Progress MUST be tracked via task lists with clear ownership and status
- MVP delivery: complete P1 stories first, validate, then proceed to P2

## Agent Team Collaboration

The project operates with a 5-role agent team. Each role has distinct
responsibilities and veto authority within their domain:

| Role | Owner | Domain Authority |
|------|-------|-----------------|
| Product Manager (PM) | Requirements & priorities | Can veto scope changes |
| Technical Architect (Arch) | System design & tech choices | Can veto architecture violations |
| Development Engineer (Dev) | Implementation & code quality | Can veto unimplementable designs |
| QA Engineer (QA) | Testing & quality assurance | Can veto releases with failing tests |
| Project Coordinator (Coord) | Process & delivery tracking | Can veto unplanned scope additions |

**Collaboration rules**:

- Cross-role decisions MUST involve at least the two most relevant roles
- Disagreements are resolved by the role with domain authority
- All roles MUST review and approve the implementation plan before coding begins
- Risk assessments require input from at least Arch, Dev, and PM

## Development Workflow

### Phase Gates

1. **Specification Gate** (PM owns): Spec passes quality checklist → Plan can begin
2. **Design Gate** (Arch owns): Architecture reviewed, contracts defined → Tasks can begin
3. **Implementation Gate** (Dev owns): Code compiles, unit tests pass → Integration can begin
4. **Quality Gate** (QA owns): All tests pass, coverage met → Release candidate ready
5. **Delivery Gate** (Coord owns): Documentation complete, risks addressed → Ship

### Review Process

- Every PR MUST be reviewed against the principle relevant to the change:
  - API changes → Arch review required
  - New features → PM verification against spec required
  - All changes → QA verification that tests cover the change
- Constitution violations MUST be flagged and resolved before merge
- Complexity additions MUST be justified in the Complexity Tracking table

## Governance

This constitution supersedes all ad-hoc practices. Amendments require:

1. Proposal with rationale documented in a meeting record
2. Agreement from at least 3 of 5 roles (must include the domain authority)
3. Version bump following semantic versioning:
   - MAJOR: Principle removal or backward-incompatible redefinition
   - MINOR: New principle or material expansion of existing guidance
   - PATCH: Clarifications, wording fixes, non-semantic refinements
4. Propagation check across all speckit templates

All PRs and reviews MUST verify compliance with these principles.
Complexity MUST be justified — simplicity is the default.

**Version**: 1.0.0 | **Ratified**: 2026-03-04 | **Last Amended**: 2026-03-04
