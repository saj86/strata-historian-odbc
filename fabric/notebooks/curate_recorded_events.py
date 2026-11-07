# Fabric notebook (PySpark). Status: Not run: no Spark or Delta environment was available.
# Plain-Python rules and their tests: curation_model.py, ../tests/test_curation_model.py.
# Parameters are set by pipeline pl_strata_recorded_ingest.
import hashlib

from pyspark.sql import SparkSession, functions as F

spark = SparkSession.builder.getOrCreate()
spark.conf.set("spark.sql.session.timeZone", "UTC")  # so collected timestamps are UTC


def event_checksum(rows):
    """rows: (EventUtcRaw, EventSeq, ValueKind, NumericValue, StateText, IsGood). Order does not matter."""
    def field_text(value):
        if value is None:
            return "\\N"
        if isinstance(value, bool):
            return "1" if value else "0"
        if isinstance(value, float):
            return repr(value)
        return str(value)
    lines = sorted("\x1f".join(field_text(v) for v in row) for row in rows)
    return "sha256:" + hashlib.sha256("\n".join(lines).encode("utf-8")).hexdigest()


def raw_matches_timestamp(raw, ts):
    """True if the typed timestamp agrees with the raw text to microseconds, Spark's finest unit."""
    text = raw.rstrip("Zz")
    whole, _, fraction = text.partition(".")
    if whole.replace("T", " ") != ts.strftime("%Y-%m-%d %H:%M:%S"):
        return False
    return (fraction + "000000")[:6] == f"{ts.microsecond:06d}"


# --- parameters cell ---
source_server = "FIXTURE-V1"
point_id = "PT-PRESSURE-001"
window_start = "2026-01-15T08:00:00Z"
window_end = "2026-01-15T09:00:00Z"
run_id = "run-0001"
attempt = 1
expected_count = 7          # None when no independent count exists
expected_checksum = None    # Set for a bounded fixture. fabric/schemas/sample_control_records.json has the fixture value.
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

def record_control(status, observed, checksum_value, error):
    """Insert or update this attempt's control record. A temp view carries the values, so NULLs and quoting are safe."""
    row = [(source_server, "recorded_events", point_id, window_start, window_end, run_id, attempt, status,
            expected_count, observed, checksum_value, error)]
    spark.createDataFrame(row, "source_server string, dataset string, point_batch_id string, window_start_utc string, "
                               "window_end_utc string, run_id string, attempt int, status string, expected_count bigint, "
                               "observed_count bigint, checksum string, error string").createOrReplaceTempView("control_update")
    spark.sql(f"""
      MERGE INTO {control_table} c USING control_update u
      ON c.source_server = u.source_server AND c.dataset = u.dataset AND c.point_batch_id = u.point_batch_id
         AND c.window_start_utc = u.window_start_utc AND c.run_id = u.run_id AND c.attempt = u.attempt
      WHEN MATCHED THEN UPDATE SET status = u.status, observed_count = u.observed_count, checksum = u.checksum, error = u.error
      WHEN NOT MATCHED THEN INSERT (source_server, dataset, point_batch_id, window_start_utc, window_end_utc, run_id, attempt, status, expected_count, observed_count, checksum, error)
        VALUES (u.source_server, u.dataset, u.point_batch_id, u.window_start_utc, u.window_end_utc, u.run_id, u.attempt, u.status, u.expected_count, u.observed_count, u.checksum, u.error)
    """)


def reject(message):
    record_control("FAILED", 0, None, message[:1000])
    raise ValueError(message)


path = f"{staging_path}/source={source_server}/point={point_id}/window={window_start.replace(':', '')}/run={run_id}/attempt={attempt}/"
df = spark.read.parquet(path)

# Validate the landed schema: schema evolution is a decision, not an accident.
actual = dict(df.dtypes)
wrong = {c: (t, actual.get(c)) for c, t in EXPECTED_COLUMNS.items() if actual.get(c) != t}
if wrong:
    reject(f"Schema mismatch, batch rejected: {wrong}")

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
    reject(f"Batch rejected: {checks.asDict()} duplicates={dupes}")
if expected_count is not None and checks["rows"] != expected_count:
    reject(f"Row count {checks['rows']} != expected {expected_count}")
# Windows are bounded, so collecting one to the driver is acceptable here.
rows = df.select("EventUtcRaw", "EventSeq", "ValueKind", "NumericValue", "StateText", "IsGood", "EventUtc").collect()
imprecise = [r["EventUtcRaw"] for r in rows if not raw_matches_timestamp(r["EventUtcRaw"], r["EventUtc"])]
if imprecise:
    reject(f"Timestamp precision lost in {len(imprecise)} rows, for example {imprecise[0]}")
checksum = event_checksum(tuple(r)[:6] for r in rows)
if expected_checksum is not None and checksum != expected_checksum:
    reject(f"Checksum {checksum} != expected {expected_checksum}")
# A production source must never receive fixture rows.
if source_server != "FIXTURE-V1" and checks["fixture_rows"]:
    reject("Fixture rows found in a production batch")

# Replace exactly this point and window, atomically. Replaying gives the same curated output.
# Corrections and deletions become visible because the whole window is replaced. A failed or
# denied read raises above and never reaches this line, so no deletion is inferred from it.
curated = (df.withColumn("window_start_utc", start_ts).withColumn("window_end_utc", end_ts)
             .withColumn("run_id", F.lit(run_id)).withColumn("attempt", F.lit(attempt)))
predicate = f"SourceServer = '{source_server}' AND PointId = '{point_id}' AND window_start_utc = '{window_start}'"
(curated.write.format("delta").mode("overwrite").option("replaceWhere", predicate)
        .partitionBy("SourceServer").saveAsTable(curated_table))

# The control record is written after the Delta commit. A crash in between leaves a committed window
# with a stale watermark; the next run repeats the same replace and then advances.
# See test_crash_between_commit_and_checkpoint.
record_control("COMMITTED", checks["rows"], checksum, None)

# Contiguous-only watermark. A window counts as done if any attempt committed it, so an earlier failed
# attempt doesn't block it. A window with no record at all is a gap, and also stops the watermark.
windows = (spark.table(control_table)
           .filter(F.col("source_server") == source_server).filter(F.col("dataset") == "recorded_events").filter(F.col("point_batch_id") == point_id)
           .groupBy("window_start_utc", "window_end_utc")
           .agg(F.max(F.when(F.col("status") == "COMMITTED", 1).otherwise(0)).alias("done"))
           .orderBy("window_start_utc").collect())
mark = None
for w in windows:
    if not w["done"] or (mark is not None and w["window_start_utc"] != mark):
        break
    mark = w["window_end_utc"]
print(f"point={point_id} rows={checks['rows']} checksum={checksum} watermark={mark}")
if mark is not None:
    spark.sql(f"""UPDATE {control_table} SET last_committed_watermark_utc = '{mark}'
                  WHERE source_server = '{source_server}' AND dataset = 'recorded_events' AND point_batch_id = '{point_id}'""")
