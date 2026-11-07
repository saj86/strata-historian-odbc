# Lab protocol: proving the live backend

Not yet run. It needs a site, an approved identity and permission to run each step. Determining the right to run
an experiment is separate from whether it would work technically.

## Preconditions

1. Written approval from the PI system owner for this lab, on a non-production server where possible.
2. Answer to the PSA question in [backend-decision.md](backend-decision.md).
3. **Before any wire-level step on a non-documented protocol: written authorization from legal counsel** that it is
   permitted under the site's agreement and applicable law (GTC §3.1, §3.2). The PI Web API lab below does not need this step.
4. A trusted reference reader chosen by the site. Record its exact version and backend, and identify what it talks to:
   Data Archive, AF, a SQL query service, a web endpoint or a reporting intermediary. Do not query the internal AF database as a reporting API.

If no reference reader exists, stop after L1 to L6 and report Gate B as Not run: comparison against a trusted reader is what makes it pass.

## Synthetic tags

| Tag | Type | Events to preload (UTC, on a date you choose) |
|---|---|---|
| `SYN.PRESSURE.PV` | Float64, kPa | Mirror `fixtures/expected_pressure_window.csv`: normal value; a **zero**; two changes as close together as the source allows; a **bad state**; a questionable or substituted value if settable; nothing in a chosen empty window; one event after that window |
| `SYN.RUNSTATE` | Digital | Running and Stopped changes plus one system state |
| `SYN.DENIED` | Float64 | Any events. The test identity gets no access |

Choose one independent holdout tag and window **after** the mapping is frozen, to validate rather than tune.

## Experiments

Run one operation at a time and change one input at a time. Record the request log (`TraceFile=` in the DSN) and the reference output.

| ID | Operation | Changed input | Expected driver result | Compare against |
|---|---|---|---|---|
| L1 | Connect, authenticate normally | Valid identity | Connect succeeds; `resolveServer` 200 | Server log |
| L1a | Connect | Wrong password | `28000` | |
| L1b | Connect | Untrusted certificate on the host | `08001`, connection refused; no bypass exists | |
| L2 | `SELECT * FROM Points WHERE TagName = 'SYN.PRESSURE.PV'` | | One row; identity and units match | Reference |
| L3 | Same, with the WebId as `PointId` through `getPoint` | | Same | |
| L4 | Bounded recorded read of the fixed window | | Timestamp, value, quality and end-of-window match event for event | **Reference: pass criterion** |
| L5 | Empty window | | Success, zero rows | Reference |
| L6 | Page boundary | `PageSize=2`, then 3, then 1000 | Identical rows and order each time | L4 result |
| L7 | Digital and system states | `SYN.RUNSTATE` | Code, label and `ValueKind` match | Reference |
| L8 | Denied read | `SYN.DENIED` | `42000` or `42S02`, never empty success. Absent from `Points` | Server log |
| L9 | Cancel during a large read | `SQLCancel` mid-fetch | `HY008`, prompt return | Connection closes |
| L10 | Rename a tag, re-read by `PointId` | | Same identity, new `TagName` | |
| L11 | Disconnect, reconnect; network drop mid-read | | Drop gives `08S01` after retries; never a short result reported as complete | |
| L12 | Holdout tag and window | | Matches reference with no code changes | **Reference: pass criterion** |

Repeat L4 three times to establish reproducibility.

## Choosing the observation layer

ODBC Driver Manager tracing shows calls from an ODBC consumer. An OLE DB-only reference needs a minimal OLE DB consumer or
provider diagnostics. Neither trace establishes a PI server's wire format. For PI Web API, the application request log plus server
logs are enough, and no packet capture is needed. With TLS, a capture would show only timing and connection behavior anyway.

## Recording results

Add one row per observation to `evidence-log.md`:

| Operation | Changed input | Observed response | Reproducibility | Client version | Server version | Interpretation | Confidence | Disproving test |
|---|---|---|---|---|---|---|---|---|

## Handling captured material

Sanitize before committing: replace hostnames, identities, tag names and WebIds. Keep vendor binaries, proprietary
source, secrets, licence artifacts and production captures out of this repository; `.gitignore` blocks common capture and key file types.
Implement only from the written specification and cite the experiment that supports each field.

A guessed field layout or simulated response must never be reported as a live read. `Backend=native` stays an error until a transaction passes this protocol.
