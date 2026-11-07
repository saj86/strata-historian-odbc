# Team rollout

**Recommended shape: one published report on a gateway, so most people install nothing.** Only report authors and gateway machines need the driver. People who read reports use the Power BI service in a browser, and see data already imported into the model.

## Who needs what

| Who | Needs | Doesn't need |
|---|---|---|
| Report authors (you and any colleague building reports) | Driver, DSN, Power BI Desktop, access to PI Web API | |
| Gateway machine (one or more) | Driver, DSN with the same name, on-premises data gateway, access to PI Web API | Power BI Desktop |
| Report readers (colleagues, Water Department) | A Power BI licence that lets them view the report, and access to the workspace or app | Driver, DSN, PI access |
| PI system | PI Web API, one least-privilege identity for refresh | Anything from this project |

Whether readers need a Pro licence depends on your Power BI capacity. Ask your Power BI administrator.

## What to decide

**Who should see what is the main decision.** Refresh reads PI with one identity, and everyone with access to the report sees that imported data, whatever their own PI permissions are.

| Situation | Do this |
|---|---|
| Everyone may see all the tags in the report | One model and one workspace or app |
| The Water Department may see only its own tags | Publish a separate model for it, or add row-level security in Power BI |
| Some tags must never leave a small group | Leave them out of the shared model, and don't give the refresh identity access to them |

Ask the Water Department's data owner to confirm which tags are in scope. Water utility data may have its own security rules, so check with your security team before publishing it widely.

## Rollout order

1. Run `Test-Package.ps1` on one test machine, with no PI server involved.
2. Open the Power BI project and confirm the sample page.
3. Send the licence question below, and wait for the answer.
4. With your PI administrator's approval, run `Test-PiWebApi.ps1` to find the version and sign-in methods.
5. Get a least-privilege PI identity for the driver, limited to the tags in scope.
6. Do the live test (Gate B): read one tag and one window, then compare the result with a tool you already trust. Confirm that a tag the identity can't read stays denied.
7. Build the MSI, sign it, and install it on one gateway. Publish and refresh (Gate C).
8. Share the report with your colleagues first, then the Water Department.

## Check PI Web API

This sends GET requests only and prints no values. Ask your PI administrator before running it on a production server.

```powershell
.\scripts\Test-PiWebApi.ps1 -BaseUrl https://YOUR-SERVER/piwebapi
.\scripts\Test-PiWebApi.ps1 -BaseUrl https://YOUR-SERVER/piwebapi -UseWindowsAuth
.\scripts\Test-PiWebApi.ps1 -BaseUrl https://YOUR-SERVER/piwebapi -UseWindowsAuth -DataServer YOUR-DATA-SERVER -TagName YOUR.TAG
```

The first command needs no login, and shows whether PI Web API is there and which sign-in methods it offers. If you don't know the address, ask your PI administrator. It's usually on the PI Web API server at `/piwebapi`.

## The licence question

Send this to whoever manages your AVEVA contract, or to your AVEVA account representative:

> We plan to read PI data for internal Power BI reports using an in-house ODBC driver, which calls PI Web API using read-only requests. It is not an AVEVA product. Reports would be read by our team and the Water Department.
>
> AVEVA's PI System software schedule (v1.5, June 2026), section 3.3(b)(iv), says a PSA license is required when deploying a non-AVEVA program that programmatically accesses data stored in the PI Server.
>
> 1. Does our agreement include PSA, and does it cover this use?
> 2. If not, what would it cost, and how is it counted (per server, per program or per user)?
> 3. Does viewing the resulting reports, without direct PI access, need any further licence?
> 4. Is PI Integrator for Business Analytics a better fit for this, and what would it cost?

Hold off using the driver on production PI data until you have the answer. Testing with the fixture and the mock doesn't touch PI, so it can go ahead now.
