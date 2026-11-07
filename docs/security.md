# Security

## Identity per hop

| Hop | Identity | Notes |
|---|---|---|
| User → Power BI service | Entra ID | Workspace roles decide who can see the model |
| Service → gateway | Gateway registration | |
| Gateway service process | `NT SERVICE\PBIEgwService` | **Not** the PI identity; it needs only local file, registry and network access |
| Gateway → driver → PI Web API | The credential in the gateway connection | This is the identity whose PI access is used |
| PI Web API → Data Archive | PI mappings for that identity | Enforced by the server, not the driver |

**A shared Import model reads with one PI identity, and everyone with access to the model sees the cached result.** PI point security doesn't apply per report viewer. Choose the identity's access to match the audience. If viewers differ in what they may see, use Power BI row-level security or separate models and workspaces. PI permissions alone will not protect imported data.

## Credentials

- Never in a DSN. The driver doesn't read `UID`/`PWD` from a DSN, `ConfigDSNW` refuses them, the provisioning script removes them, and the health check fails on them.
- Stored in the gateway connection, encrypted by the service. Desktop stores them in its own credential store.
- Not in M queries, not in the output connection string (redaction tested), not in the trace (tested).
- Rotation: change the PI identity's password, update the gateway connection and Desktop, refresh, then check history. Rotate on a schedule; test it in Gate E.

## Authentication

Derived from the exact route, not assumed.

| Route | Options | Source |
|---|---|---|
| Built-in connector | Database, Windows, Default or Custom | Power Query ODBC page |
| Custom `.mez` through the gateway | **Kerberos with Windows authentication isn't supported** | Microsoft custom connector page |
| Fabric Dataflow Gen2 | Anonymous, Basic, Windows | Fabric connector table |
| Fabric Pipeline Copy, Copy job | Anonymous, Basic | Fabric connector table |

`Auth=Negotiate` sends the identity of the current process or impersonated thread, using WinHTTP's low-security autologon policy, which sends credentials to whatever host `BaseUrl` names. Use it only with a specific, trusted `BaseUrl`. On the gateway, confirm through the PI Web API server log which identity arrives.

**Do not pair `Auth=Negotiate` in a DSN with a Fabric "Anonymous" connection.** It would silently act as the gateway service account, which reinterprets Anonymous as Windows and could widen PI access. The driver can't detect this from inside, so a change-control rule and the server log are the controls.

Basic sends a reusable password on each request, so it relies entirely on TLS. PI Web API must have Basic enabled by its administrator, and that's a security decision for them.

## Transport

`https` is required; plain `http` is accepted only for loopback test servers. Certificate validation stays at the Windows default, with **no option to disable it**. A failure returns `08001`; fix the trust store on the host. TLS 1.2 minimum. Allow the following flows, and no others from these hosts for this feature:

| From | To | Port | Purpose |
|---|---|---|---|
| Workstations, gateway members | PI Web API host | 443 or the site's port | Driver |
| Gateway members | Azure Service Bus and Power BI endpoints | Per Microsoft's gateway documentation | Gateway |
| Gateway members (Fabric pipelines) | `*.frontend.clouddatahub.net` | Outbound | Per Microsoft's Fabric gateway page |

## Authorization

Denied values and denied metadata are both tested with the fixture: a denied point doesn't appear in `Points`, and reading it fails. Gate B repeats this on a real server, and must include a **gateway identity with different access from the developer's**, to show that a developer's broader access isn't what makes refresh succeed.

## Logging

`TraceFile` is off by default. It holds point IDs, time windows, row counts, HTTP status, timing and correlation IDs. It never holds credentials, tokens or values. Point IDs can be sensitive, so protect the folder and set retention. Error messages avoid URLs and hostnames, but include point IDs. Don't attach traces to tickets without review.

## Read-only guarantee

Writes are blocked in three places: the parser refuses them, the only HTTP method in the transport is `GET`, and `SQL_DATA_SOURCE_READ_ONLY` reports `Y`. Parameter values are bound values and are URL-encoded before use, so SQL text never reaches PI.
