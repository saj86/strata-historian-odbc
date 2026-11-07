using System.IO.Compression;
using System.Text;
using StrataHistorian.Setup.Engine;
using Xunit;

namespace StrataHistorian.Setup.Tests;

public sealed class EngineTests
{
    private const string Dir = @"C:\Program Files\Strata Historian ODBC";
    private const string Self = @"C:\Downloads\strata-historian-odbc-setup-0.2.0.exe";

    private static IPayload Payload(string version = "0.2.0")
    {
        using var buffer = new MemoryStream();
        using (var zip = new ZipArchive(buffer, ZipArchiveMode.Create, leaveOpen: true))
        {
            void Add(string path, string text)
            {
                using var writer = new StreamWriter(zip.CreateEntry(path).Open());
                writer.Write(text);
            }

            Add("strataodbc.dll", "dll " + version);
            Add("VERSION.txt", version + "\n");
            Add("License.txt", "licence");
            Add("docs/usage-guide.md", "guide");
            Add("samples/powershell/Example.ps1", "example");
        }

        buffer.Position = 0;
        return Engine.Payload.FromZip(buffer);
    }

    private static (MemorySystem System, List<string> Log, Installer Installer) Machine(IPayload? payload = null)
    {
        var system = new MemorySystem();
        system.WriteFile(Self, [9, 9]);
        var log = new List<string>();
        var installer = new Installer(system, payload ?? Payload(), (_, text) => log.Add("==> " + text), log.Add) { SelfPath = Self };
        return (system, log, installer);
    }

    private static SetupAnswers Answers(Action<DataSourceSettings>? change = null)
    {
        var answers = new SetupAnswers { InstallDir = Dir };
        answers.DataSource.BaseUrl = "https://piweb.example.local/piwebapi";
        answers.DataSource.DataServer = "PISRV01";
        change?.Invoke(answers.DataSource);
        return answers;
    }

    [Fact]
    public void PayloadReadsVersionAndRefusesPathsOutsideTheFolder()
    {
        Assert.Equal("0.2.0", Payload().Version);
        using var buffer = new MemoryStream();
        using (var zip = new ZipArchive(buffer, ZipArchiveMode.Create, leaveOpen: true))
        {
            zip.CreateEntry("strataodbc.dll");
            zip.CreateEntry("../evil.dll");
        }

        buffer.Position = 0;
        Assert.Throws<InvalidDataException>(() => Engine.Payload.FromZip(buffer));
    }

    [Fact]
    public void TheBuiltPayloadIsComplete()
    {
        // scripts/build-setup.sh prints the payload it built; run the tests with STRATA_PAYLOAD set to check it.
        var path = Environment.GetEnvironmentVariable("STRATA_PAYLOAD");
        if (string.IsNullOrEmpty(path))
        {
            return;
        }

        using var file = File.OpenRead(path);
        var payload = Engine.Payload.FromZip(file);
        Assert.Matches(@"^\d+\.\d+\.\d+$", payload.Version);
        foreach (var required in new[] { "strataodbc.dll", "VERSION.txt", "License.txt", @"docs\usage-guide.md", @"samples\powershell\Example.ps1", @"samples\powerbi\project\StrataHistorian.pbip" })
        {
            Assert.Contains(payload.Files, f => f.Path.Equals(required, StringComparison.OrdinalIgnoreCase));
        }

        var dll = payload.Files.Single(f => f.Path == "strataodbc.dll").Read();
        Assert.Equal((byte)'M', dll[0]);
        Assert.Equal((byte)'Z', dll[1]);
        Assert.DoesNotContain(payload.Files, f => f.Path.Contains(@"\bin\", StringComparison.OrdinalIgnoreCase) || f.Path.Contains(@"\obj\", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void InstallRegistersTheDriverExactlyAsTheMsiDoes()
    {
        var (system, _, installer) = Machine();
        installer.Install(Answers());

        var key = @"SOFTWARE\ODBC\ODBCINST.INI\Strata Historian ODBC Driver";
        Assert.Equal(Dir + @"\strataodbc.dll", system.ReadValue(key, "Driver"));
        Assert.Equal(Dir + @"\strataodbc.dll", system.ReadValue(key, "Setup"));
        Assert.Equal("03.52", system.ReadValue(key, "DriverODBCVer"));
        Assert.Equal("YYN", system.ReadValue(key, "ConnectFunctions"));
        Assert.Equal("Installed", system.ReadValue(@"SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers", "Strata Historian ODBC Driver"));
        Assert.True(system.FileExists(Dir + @"\strataodbc.dll"));
        Assert.True(system.FileExists(Dir + @"\docs\usage-guide.md"));
        Assert.True(system.FileExists(Dir + @"\StrataSetup.exe"));

        var state = InstallState.Read(system);
        Assert.True(state.Installed);
        Assert.False(state.RegisteredElsewhere);
        Assert.Equal("0.2.0", state.Version);
    }

    [Fact]
    public void InstallAddsAProgramsAndFeaturesEntryAndShortcuts()
    {
        var (system, _, installer) = Machine();
        installer.Install(Answers());

        var arp = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\StrataHistorianOdbc";
        Assert.Equal("Strata Historian ODBC Driver", system.ReadValue(arp, "DisplayName"));
        Assert.Equal("0.2.0", system.ReadValue(arp, "DisplayVersion"));
        Assert.Equal("\"" + Dir + "\\StrataSetup.exe\" --uninstall", system.ReadValue(arp, "UninstallString"));
        Assert.Equal("1", system.ReadValue(arp, "NoRepair"));

        var menu = @"C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Strata Historian ODBC\";
        Assert.Equal(@"C:\Windows\System32\odbcad32.exe", system.Shortcuts[menu + "ODBC Data Sources (64-bit).lnk"]);
        Assert.Equal(Dir + @"\StrataSetup.exe", system.Shortcuts[menu + "Change or remove Strata Historian ODBC Driver.lnk"]);
        Assert.Contains(menu + "Usage guide.lnk", system.Shortcuts.Keys);
    }

    [Fact]
    public void InstallCreatesTheDataSourceAndItsOleDbLinkWithoutCredentials()
    {
        var (system, _, installer) = Machine();
        var answers = Answers(s => s.Auth = "Basic");
        answers.TestUser = "me";
        answers.TestPassword = "secret";
        installer.Install(answers);

        var key = @"SOFTWARE\ODBC\ODBC.INI\StrataPI";
        Assert.Equal(Dir + @"\strataodbc.dll", system.ReadValue(key, "Driver"));
        Assert.Equal("piwebapi", system.ReadValue(key, "Backend"));
        Assert.Equal("https://piweb.example.local/piwebapi", system.ReadValue(key, "BaseUrl"));
        Assert.Equal("Basic", system.ReadValue(key, "Auth"));
        Assert.Equal("Strata Historian ODBC Driver", system.ReadValue(@"SOFTWARE\ODBC\ODBC.INI\ODBC Data Sources", "StrataPI"));
        Assert.DoesNotContain(system.ValueNames(key), name => name is "UID" or "PWD");
        Assert.DoesNotContain(system.Files.Select(system.ReadFile), bytes => Encoding.UTF8.GetString(bytes).Contains("secret", StringComparison.Ordinal) || Encoding.Unicode.GetString(bytes).Contains("secret", StringComparison.Ordinal));

        var udl = @"C:\Users\Public\Documents\Strata Historian ODBC\StrataPI.udl";
        var bytes = system.ReadFile(udl);
        Assert.Equal([0xFF, 0xFE], bytes[..2]);
        var text = Encoding.Unicode.GetString(bytes, 2, bytes.Length - 2);
        Assert.StartsWith("[oledb]\r\n", text, StringComparison.Ordinal);
        Assert.Contains("Provider=MSDASQL.1;Persist Security Info=False;Data Source=StrataPI\r\n", text, StringComparison.Ordinal);
        Assert.Equal(["StrataPI"], InstallState.Read(system).DataSources);
    }

    [Fact]
    public void TestDataSourceClearsServerSettings()
    {
        var (system, _, installer) = Machine();
        installer.Install(Answers(s => { s.TestData = true; s.Name = "StrataFixture"; s.OleDbLink = false; }));
        var key = @"SOFTWARE\ODBC\ODBC.INI\StrataFixture";
        Assert.Equal("fixture", system.ReadValue(key, "Backend"));
        Assert.Null(system.ReadValue(key, "BaseUrl"));
        Assert.Null(system.ReadValue(key, "Auth"));
        Assert.False(system.FileExists(@"C:\Users\Public\Documents\Strata Historian ODBC\StrataFixture.udl"));
    }

    [Fact]
    public void OptionalPartsCanBeLeftOut()
    {
        var (system, _, installer) = Machine();
        var answers = Answers();
        answers.InstallDocs = false;
        answers.Shortcuts = false;
        answers.CreateDataSource = false;
        installer.Install(answers);
        Assert.False(system.FileExists(Dir + @"\docs\usage-guide.md"));
        Assert.Empty(system.Shortcuts);
        Assert.Empty(InstallState.Read(system).DataSources);
        Assert.True(system.FileExists(Dir + @"\strataodbc.dll"));
    }

    [Fact]
    public void ADriverRegisteredByTheMsiIsNotTakenOver()
    {
        var (system, _, installer) = Machine();
        system.WriteString(@"SOFTWARE\ODBC\ODBCINST.INI\Strata Historian ODBC Driver", "Driver", @"C:\Program Files\Other\strataodbc.dll");
        var state = InstallState.Read(system);
        Assert.True(state.RegisteredElsewhere);
        var error = Assert.Throws<InvalidOperationException>(() => installer.Install(Answers()));
        Assert.Contains("MSI", error.Message, StringComparison.Ordinal);
        Assert.Equal(@"C:\Program Files\Other\strataodbc.dll", system.ReadValue(@"SOFTWARE\ODBC\ODBCINST.INI\Strata Historian ODBC Driver", "Driver"));
    }

    [Fact]
    public void UpdateReplacesFilesAndKeepsDataSourcesAndChoices()
    {
        var (system, _, installer) = Machine(Payload("0.2.0"));
        var answers = Answers();
        answers.InstallDocs = false;
        installer.Install(answers);
        system.WriteString(@"SOFTWARE\ODBC\ODBC.INI\StrataPI", "MaxRows", "500");

        var (_, _, newer) = (system, new List<string>(), new Installer(system, Payload("0.3.0"), (_, _) => { }, _ => { }) { SelfPath = Self });
        newer.Update(new SetupAnswers());
        Assert.Equal("dll 0.3.0", Encoding.UTF8.GetString(system.ReadFile(Dir + @"\strataodbc.dll")));
        Assert.Equal("0.3.0", InstallState.Read(system).Version);
        Assert.Equal("500", system.ReadValue(@"SOFTWARE\ODBC\ODBC.INI\StrataPI", "MaxRows"));
        Assert.False(system.FileExists(Dir + @"\docs\usage-guide.md"));
        Assert.Throws<InvalidOperationException>(() => newer.Install(Answers()));
    }

    [Fact]
    public void ADriverInUseGivesAClearMessage()
    {
        var (system, _, installer) = Machine();
        installer.Install(Answers());
        system.LockedFile = Dir + @"\strataodbc.dll";
        var error = Assert.Throws<InvalidOperationException>(() => installer.Update(new SetupAnswers()));
        Assert.Contains("in use", error.Message, StringComparison.Ordinal);
    }

    [Fact]
    public void TheGatewayIsStoppedAndStartedAgainEvenWhenTheRunFails()
    {
        var (system, _, installer) = Machine();
        system.Services["PBIEgwService"] = "Running";
        installer.Install(Answers());
        Assert.Equal(["stop PBIEgwService", "start PBIEgwService"], system.ServiceLog);

        system.LockedFile = Dir + @"\strataodbc.dll";
        Assert.Throws<InvalidOperationException>(() => installer.Update(new SetupAnswers()));
        Assert.Equal("Running", system.Services["PBIEgwService"]);

        var (quiet, _, other) = Machine();
        quiet.Services["PBIEgwService"] = "Running";
        var answers = Answers();
        answers.RestartGateway = false;
        other.Install(answers);
        Assert.Empty(quiet.ServiceLog);
    }

    [Fact]
    public void ConfigureRenamesADataSourceAndRefusesOneOwnedByAnotherDriver()
    {
        var (system, _, installer) = Machine();
        installer.Install(Answers());
        var rename = new SetupAnswers { EditingDataSource = "StrataPI", DataSource = DataSourceSettings.Read(system, "StrataPI") };
        rename.DataSource.Name = "WaterPI";
        installer.Configure(rename);
        Assert.Equal(["WaterPI"], InstallState.Read(system).DataSources);
        Assert.False(system.FileExists(@"C:\Users\Public\Documents\Strata Historian ODBC\StrataPI.udl"));
        Assert.True(system.FileExists(@"C:\Users\Public\Documents\Strata Historian ODBC\WaterPI.udl"));
        Assert.Equal("https://piweb.example.local/piwebapi", DataSourceSettings.Read(system, "WaterPI").BaseUrl);

        system.WriteString(@"SOFTWARE\ODBC\ODBC.INI\ODBC Data Sources", "SqlServer", "ODBC Driver 18 for SQL Server");
        var clash = new SetupAnswers { DataSource = DataSourceSettings.Read(system, "WaterPI") };
        clash.DataSource.Name = "SqlServer";
        Assert.Throws<InvalidOperationException>(() => installer.Configure(clash));
    }

    [Fact]
    public void ConfigureRemovesValuesAddedByHandSuchAsAPassword()
    {
        var (system, _, installer) = Machine();
        installer.Install(Answers());
        system.WriteString(@"SOFTWARE\ODBC\ODBC.INI\StrataPI", "PWD", "leaked");
        installer.Configure(new SetupAnswers { EditingDataSource = "StrataPI", DataSource = DataSourceSettings.Read(system, "StrataPI") });
        Assert.Null(system.ReadValue(@"SOFTWARE\ODBC\ODBC.INI\StrataPI", "PWD"));
    }

    [Fact]
    public void UninstallRemovesEverythingButKeepsDataSourcesUnlessAsked()
    {
        var (system, log, installer) = Machine();
        installer.Install(Answers());
        installer.Uninstall(new SetupAnswers());
        Assert.False(InstallState.Read(system).Installed);
        Assert.Null(system.ReadValue(@"SOFTWARE\ODBC\ODBCINST.INI\Strata Historian ODBC Driver", "Driver"));
        Assert.Null(system.ReadValue(@"SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers", "Strata Historian ODBC Driver"));
        Assert.Null(system.ReadValue(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\StrataHistorianOdbc", "DisplayName"));
        Assert.False(system.FileExists(Dir + @"\strataodbc.dll"));
        Assert.Empty(system.Shortcuts);
        Assert.Equal(["StrataPI"], InstallState.Read(system).DataSources);
        Assert.Contains(log, line => line.Contains("Kept data sources: StrataPI", StringComparison.Ordinal));

        var (again, _, second) = Machine();
        second.Install(Answers());
        second.Uninstall(new SetupAnswers { RemoveDataSources = true });
        Assert.Empty(InstallState.Read(again).DataSources);
        Assert.False(again.FileExists(@"C:\Users\Public\Documents\Strata Historian ODBC\StrataPI.udl"));
    }

    [Theory]
    [InlineData("", "Enter a data source name.")]
    [InlineData("a;b", "can't contain")]
    [InlineData("123456789012345678901234567890123", "at most 32")]
    public void DataSourceNamesAreChecked(string name, string expected)
    {
        var settings = Answers(s => s.Name = name).DataSource;
        Assert.Contains(expected, settings.Validate() ?? string.Empty, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("http://piweb.example.local/piwebapi", "https://")]
    [InlineData("https://user:pass@piweb.example.local/piwebapi", "must not contain")]
    [InlineData("not a url", "Enter the PI Web API URL")]
    public void AddressesAreChecked(string url, string expected)
    {
        var settings = Answers(s => s.BaseUrl = url).DataSource;
        Assert.Contains(expected, settings.Validate() ?? string.Empty, StringComparison.Ordinal);
    }

    [Fact]
    public void LimitsAndServerAreChecked()
    {
        Assert.Null(Answers().DataSource.Validate());
        Assert.NotNull(Answers(s => s.PageSize = 0).DataSource.Validate());
        Assert.NotNull(Answers(s => s.MaxWindowDays = 4000).DataSource.Validate());
        Assert.NotNull(Answers(s => s.DataServer = " ").DataSource.Validate());
        Assert.Null(Answers(s => { s.TestData = true; s.BaseUrl = string.Empty; s.DataServer = string.Empty; }).DataSource.Validate());
        Assert.Null(Answers(s => s.BaseUrl = "http://127.0.0.1:8765/piwebapi").DataSource.Validate());
    }

    [Fact]
    public void TestConnectionStringCarriesCredentialsOnlyForTheTest()
    {
        var settings = Answers(s => s.Auth = "Basic").DataSource;
        var text = settings.TestConnectionString("me", "p;w}d");
        Assert.StartsWith("DRIVER={Strata Historian ODBC Driver};Backend=piwebapi;BaseUrl=https://piweb.example.local/piwebapi;", text, StringComparison.Ordinal);
        Assert.Contains("UID=me;PWD={p;w}}d};", text, StringComparison.Ordinal);
        Assert.DoesNotContain("PWD", settings.TestConnectionString(string.Empty, string.Empty), StringComparison.Ordinal);
    }
}
