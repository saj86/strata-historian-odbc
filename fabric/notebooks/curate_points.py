# Fabric notebook. Status: Not run. Rules and tests: merge_points in curation_model.py.
# Staging holds the output of SELECT * FROM Points, copied by the pipeline in the same way as recorded events.
from pyspark.sql import SparkSession, functions as F

spark = SparkSession.builder.getOrCreate()

# --- parameters cell ---
source_server = "FIXTURE-V1"
run_id = "run-0001"
attempt = 1
staging_path = "Files/staging/points"
curated_table = "curated_points"

df = spark.read.parquet(f"{staging_path}/source={source_server}/run={run_id}/attempt={attempt}/")

rows = df.count()
if rows == 0:
    raise ValueError("A complete read returned no points. This looks like a permissions change, so nothing was changed.")
if df.filter(F.col("SourceServer") != source_server).count():
    raise ValueError("Batch rejected: rows from another source")
if df.groupBy("PointId").count().filter("count > 1").count():
    raise ValueError("Batch rejected: duplicate PointId")
if source_server != "FIXTURE-V1" and df.filter(F.col("SourceBackend") == "fixture").count():
    raise ValueError("Fixture rows found in a production batch")

df.withColumn("last_seen_run_id", F.lit(run_id)).createOrReplaceTempView("points_update")
# A point missing from a complete read goes inactive and stays, because history still refers to it.
spark.sql(f"""
  MERGE INTO {curated_table} c USING points_update u
  ON c.SourceServer = u.SourceServer AND c.PointId = u.PointId
  WHEN MATCHED THEN UPDATE SET
    SourceBackend = u.SourceBackend, TagName = u.TagName, Descriptor = u.Descriptor, PointClass = u.PointClass, PointType = u.PointType,
    EngUnits = u.EngUnits, DigitalSetName = u.DigitalSetName, IsStep = u.IsStep, IsActive = true, last_seen_run_id = u.last_seen_run_id
  WHEN NOT MATCHED THEN INSERT
    (SourceBackend, SourceServer, PointId, TagName, Descriptor, PointClass, PointType, EngUnits, DigitalSetName, IsStep, IsActive, last_seen_run_id)
    VALUES (u.SourceBackend, u.SourceServer, u.PointId, u.TagName, u.Descriptor, u.PointClass, u.PointType, u.EngUnits, u.DigitalSetName, u.IsStep, true, u.last_seen_run_id)
  WHEN NOT MATCHED BY SOURCE AND c.SourceServer = '{source_server}' THEN UPDATE SET IsActive = false
""")
print(f"source={source_server} points={rows}")
