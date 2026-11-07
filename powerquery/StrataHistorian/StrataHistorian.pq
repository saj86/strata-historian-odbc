// OPTIONAL, SEPARATE ARTIFACT. Not needed for Import through the built-in ODBC connector.
// Status: Not run. It has not been compiled with the Power Query SDK or loaded in Power BI.
// DirectQuery stays OFF here until the visual, filter, grouping and concurrency tests pass.
// Gateway note: Microsoft states that Kerberos with Windows authentication is not supported for
// custom connectors through the on-premises gateway, so this exposes Basic and Anonymous only.
section StrataHistorian;

[DataSource.Kind = "StrataHistorian", Publish = "StrataHistorian.Publish"]
shared StrataHistorian.Contents = Value.ReplaceType(ContentsImpl, ContentsType);

ContentsType = type function (dsn as (type text meta [Documentation.FieldCaption = "System DSN"])) as table;

ContentsImpl = (dsn as text) as table =>
    let
        ConnectionString = [DSN = dsn],
        Credential = Extension.CurrentCredential(),
        CredentialString = if Credential[AuthenticationKind]? = "UsernamePassword"
            then [UID = Credential[Username], PWD = Credential[Password]]
            else [],
        Options = [
            HierarchicalNavigation = true,
            ClientConnectionPooling = true,
            CredentialConnectionString = CredentialString,
            // Truthful overrides: this mirrors SQLGetInfo. Nothing here enables folding the driver cannot run.
            SqlCapabilities = [
                SupportsTop = true,
                SupportsNumericLiterals = true,
                SupportsStringLiterals = true,
                SupportsOdbcTimestampLiterals = true,
                SupportsOdbcDateLiterals = true,
                Sql92Conformance = 1,
                GroupByCapabilities = 0
            ],
            SQLGetInfo = [SQL_AGGREGATE_FUNCTIONS = 0, SQL_SQL92_RELATIONAL_JOIN_OPERATORS = 0]
        ]
    in
        Odbc.DataSource(ConnectionString, Options);

StrataHistorian = [
    // Required for gateway refresh of a custom connector. Executes metadata access only.
    TestConnection = (dataSourcePath) => {"StrataHistorian.Contents", Json.Document(dataSourcePath)[dsn]},
    Authentication = [UsernamePassword = [], Implicit = []],
    Label = "Strata Historian"
];

StrataHistorian.Publish = [
    Beta = true,
    Category = "Other",
    ButtonText = {"Strata Historian", "Connect through a Strata Historian ODBC DSN"},
    SupportsDirectQuery = false
];
