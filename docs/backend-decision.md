# Backend decision

## Recommendation

**Use PI Web API as the live backend, and ship the offline fixture backend alongside it.** Keep the native
protocol adapter out of the build path: `Backend=native` returns an explicit `unsupported-backend` error.

PI Web API is the only option that can be implemented from a documented interface, works with a clean client
that has no PI client software, and needs no reverse engineering. It is still **unverified against a real
server**, because no site endpoint was available.

## Decide this first: the zero-spend assumption may not hold

The [AVEVA PI System software schedule, v1.5, June 2026](https://www.aveva.com/content/dam/aveva/documents/legal/policies/AVEVA-Software-Schedule-PI-System-v1-5-June-2026.pdf)
says, at §3.3(b)(iv), under "PI System Access (PSA)":

> A PSA license is required: When deploying a non-AVEVA Software program, application, or device that
> programmatically accesses data stored in the PI Server or supporting servers.

Our driver is a non-AVEVA program that programmatically reads PI Server data. On a plain reading this applies
**whichever backend or protocol we choose**. Avoiding the PI OLE DB provider does not by itself avoid it.

This is an engineering reading of a public document and not legal advice. Your own Order Form and agreement
take precedence over a public schedule, and your site may already hold this licence. **Ask whoever owns the
AVEVA contract:** does the site already hold PSA, and does it cover this deployment? §3.5 prices PI Integrators
separately by actively published data streams; if PSA is unavailable, that is the licensed alternative to compare.

## Options assessed in evidence order

| Backend | Evidence available now | Missing | Decision |
|---|---|---|---|
| PI Web API | Public AVEVA reference (recorded-values semantics seen through search excerpts); routes and parameters confirmed in a public generated client ([evidence log](evidence-log.md)) | Deployed version, allowed use, authentication scheme, test endpoint, one approved point | **Selected.** Metadata lookup and bounded recorded read implemented |
| Native protocol adapter | None. No reference client, target version or authorized capture. Contract risk below | All of it, plus written authorization | **Not implemented.** Explicit unsupported error |
| Existing reporting feed | None. No feed identified | Lineage, precision, coverage, permissions, correction policy | Not implemented. Add behind the same interface only if the site offers one |
| Offline fixture | Complete | Nothing | **Delivered.** Always labelled `SourceBackend='fixture'` |

### Why the native adapter is not simply "next"

[AVEVA's published General Terms, v3.0, September 2023](https://www.aveva.com/content/dam/aveva/documents/legal/policies/AVEVA-GTC-Website-and-Clickwrap-version_v3-0_15September2023.pdf) §3 says that,
"Unless expressly required by Applicable Law without the possibility of contractual waiver", the customer shall not
"reverse engineer, reproduce, decompile, recompile, disassemble, merge, modify, adapt or translate a Product or any
component thereof". §3.2 also bars working around a product's technical limitations.

Black-box wire observation of a licensed product may fall inside that clause, and an interoperability right would
have to come from applicable law. So the [lab protocol](lab-protocol.md) makes **written authorization from legal
counsel a precondition** for any wire-level step. The Web API lab needs no such step. Your signed agreement may differ from the published terms.

## External dependencies and costs to verify

These are questions, not findings. None could be answered from here.

| Item | Why it matters | Who can answer |
|---|---|---|
| PSA licence (§3.3(b)(iv)) | Possible new spend, independent of protocol | AVEVA contract owner |
| Is PI Web API deployed, and which version? | Backend availability; the field contract is version-dependent | PI administrator |
| Which authentication: Basic, Kerberos/NTLM, OIDC? | Decides the Fabric route (below) | PI administrator |
| PI Web API host, TLS certificate, network path from each gateway | Firewall change; trust roots on every node | PI and network administrators |
| A least-privilege PI identity and PI mappings | Cached data is visible to report readers | PI administrator, data owner |
| Server configuration limits, including the maximum count per request | `PageSize` must stay below it | PI administrator |
| Power BI licensing, gateway hosts | Existing capacity or new cost | Power BI administrator |
| Code-signing certificate | Signed MSI requirement | Security team |
| Fabric capacity (later) | Later cost only | Fabric administrator |

Server-side prerequisite if selected: a PI Web API instance must exist. Reimplementing a client does not remove
it, and installing one is a separate licensing and infrastructure decision.

## How the choice constrains authentication

| Site reality | Power BI gateway Import | Fabric route |
|---|---|---|
| PI Web API allows Basic | Database credentials in the gateway connection | Pipeline Copy possible |
| Windows-integrated only | Windows credentials through the **built-in** connector; test it | **Dataflow Gen2** only, per the connector table. Do not reinterpret Anonymous as Windows |
| Custom `.mez` needed and Windows-integrated | Blocked: Microsoft states Kerberos with Windows authentication isn't supported for custom connectors via the gateway | As above |
| OIDC only | Not implemented | Not implemented; needs a design decision |

## What Gate B needs from you

The exact missing inputs: (1) answer to the PSA question; (2) PI Web API base URL and version; (3) authentication
scheme and a test identity; (4) one authorized point, one denied point and a fixed window, or permission to create
the synthetic tags in [lab-protocol.md](lab-protocol.md); (5) a site-approved trusted reader for comparison, and its version.
