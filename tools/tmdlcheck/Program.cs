using Microsoft.AnalysisServices.Tabular;
var db = TmdlSerializer.DeserializeDatabaseFromFolder(args[0]);
Console.WriteLine($"TMDL OK: compat {db.CompatibilityLevel}, tables {db.Model.Tables.Count}, relationships {db.Model.Relationships.Count}, expressions {db.Model.Expressions.Count}");
foreach (var t in db.Model.Tables) Console.WriteLine($"  {t.Name}: {t.Columns.Count} columns (incl. RowNumber), {t.Measures.Count} measures, partition {t.Partitions.FirstOrDefault()?.Mode}");
