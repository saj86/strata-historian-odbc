# Fabric notebook (PySpark). Status: Not run: no Spark or Delta environment was available.
# Plain-Python rules and their tests: curation_model.py, ../tests/test_curation_model.py.
# Parameters are set by pipeline pl_strata_recorded_ingest.
from pyspark.sql import SparkSession, functions as F

spark = SparkSession.builder.getOrCreate()

# --- parameters cell ---
source_server = "FIXTURE-V1"
point_id = "PT-PRESSURE-001"
window_start = "2026-01-15T08:00:00Z"
window_end = "2026-01-15T09:00:00Z"
run_id = "run-0001"
attempt = 1
expected_count = 7          # None when no independent count exists
staging_path = "Files/staging/recorded_events"
curated_table = "curated_recorded_events"
control_table = "control_extraction"

EXPECTED_COLUMNS = {
    "SourceBackend": "string", "SourceServer": "string", "PointId": "string", "TagName": "string",
    "EventUtc": "timestamp", "EventUtcRaw": "string", "EventSeq": "int", "RetrievalMode": "string",
    "ValueKind": "string", "NumericValue": "double", "TextValue": "string", "DigitalCode": "int", "StateText": "string",
    "EngUnits": "string", "IsGood": "boolean", "IsQuestionable": "boolean", "IsSubstituted": "boolean", "IsAnnotated": "boolean",
    "ResultScope": "string",
}

path = f"{staging_path}/source={source_server}/point={point_id}/window={window_start.replace(':', '')}/run={run_id}/attempt={attempt}/"
df = spark.read.parquet(path)

# Validate the landed schema: schema evolution is a decision, not an accident.
actual = dict(df.dtypes)
wrong = {c: (t, actual.get(c)) for c, t in EXPECTED_COLUMNS.items() if actual.get(c) != t}
if wrong:
    raise ValueError(f"Schema mismatch, batch rejected: {wrong}")

start_ts, end_ts = F.to_timestamp(F.lit(window_start)), F.to_timestamp(F.lit(window_end))
checks = df.agg(
    F.count(F.lit(1)).alias("rows"),
    F.sum(F.when(F.col("ResultScope") != "BOUNDED", 1).otherwise(0)).alias("preview_rows"),
    F.sum(F.when((F.col("EventUtc") < start_ts) | (F.col("EventUtc") >= end_ts), 1).otherwise(0)).alias("outside"),
    F.sum(F.when(F.col("SourceServer") != source_server, 1).otherwise(0)).alias("wrong_source"),
    F.sum(F.when(F.col("PointId") != point_id, 1).otherwise(0)).alias("wrong_point"),
    F.sum(F.when(F.col("SourceBackend") == "fixture", 1).otherwise(0)).alias("fixture_rows"),
).first()
dupes = df.groupBy("PointId", "EventUtcRaw", "EventSeq").count().filter("count > 1").count()
if checks["preview_rows"] or checks["outside"] or checks["wrong_source"] or checks["wrong_point"] or dupes:
    raise ValueError(f"Batch rejected: {checks.asDict()} duplicates={dupes}")
if expected_count is not None and checks["rows"] != expected_count:
    raise ValueError(f"Row count {checks['rows']} != expected {expected_count}")
# A production source must never receive fixture rows.
if source_server != "FIXTURE-V1" and checks["fixture_rows"]:
    raise ValueError("Fixture rows found in a production batch")

# Replace exactly this point and window, atomically. Replaying gives the same curated output.
# Corrections and deletions become visible because the whole window is replaced. A failed or
# denied read raises above and never reaches this line, so no deletion is inferred from it.
curated = (df.withColumn("window_start_utc", start_ts).withColumn("window_end_utc", end_ts)
             .withColumn("run_id", F.lit(run_id)).withColumn("attempt", F.lit(attempt)))
predicate = f"SourceServer = '{source_server}' AND PointId = '{point_id}' AND window_start_utc = '{window_start}'"
(curated.write.format("delta").mode("overwrite").option("replaceWhere", predicate)
        .partitionBy("SourceServer").saveAsTable(curated_table))

# The control record is written last. A crash here leaves a committed window with a stale watermark;
# the next run repeats the same replace and then advances. See test_crash_between_commit_and_checkpoint.
spark.sql(f"""
  MERGE INTO {control_table} c
  USING (SELECT '{source_server}' s, 'recorded_events' d, '{point_id}' p, '{window_start}' w) x
  ON c.source_server = x.s AND c.dataset = x.d AND c.point_batch_id = x.p AND c.window_start_utc = x.w
  WHEN MATCHED THEN UPDATE SET status = 'COMMITTED', run_id = '{run_id}', attempt = {attempt}, observed_count = {checks['rows']}
""")

# Contiguous-only watermark.
windows = (spark.table(control_table)
           .filter(f"source_server = '{source_server}' AND dataset = 'recorded_events' AND point_batch_id = '{point_id}'")
           .orderBy("window_start_utc").collect())
mark = None
for w in windows:
    if w["status"] != "COMMITTED":
        break
    mark = w["window_end_utc"]
print(f"point={point_id} rows={checks['rows']} watermark={mark}")
if mark is not None:
    spark.sql(f"""UPDATE {control_table} SET last_committed_watermark_utc = '{mark}'
                  WHERE source_server = '{source_server}' AND dataset = 'recorded_events' AND point_batch_id = '{point_id}'""")
