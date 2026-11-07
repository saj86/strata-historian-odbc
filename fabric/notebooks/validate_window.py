# Fabric notebook nb_validate_window. Status: Not run. The function itself is tested in curation_model.py.
# Only the values returned here reach the Copy query, because Copy cannot be assumed to bind ? parameters.
import json
import re
from datetime import datetime

from notebookutils import mssparkutils
from pyspark.sql import SparkSession, functions as F

spark = SparkSession.builder.getOrCreate()

# --- parameters cell ---
source_server = "FIXTURE-V1"
point_id = "PT-PRESSURE-001"
window_start = "2026-01-15T08:00:00Z"
window_end = "2026-01-15T09:00:00Z"
max_window_days = 31        # keep equal to MaxWindowDays in the gateway DSN
control_table = "control_extraction"


def validate_window_params(point_id, start, end, known_points, max_days):
    """Returns the only values allowed into the Copy query text, or raises ValueError."""
    if point_id not in known_points:
        raise ValueError("point is not in the control table")
    pattern = r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z"
    if not re.fullmatch(pattern, start) or not re.fullmatch(pattern, end):
        raise ValueError("bounds must look like 2026-01-15T08:00:00Z")
    first = datetime.strptime(start, "%Y-%m-%dT%H:%M:%SZ")
    last = datetime.strptime(end, "%Y-%m-%dT%H:%M:%SZ")
    if last <= first:
        raise ValueError("window end must be after its start")
    if (last - first).total_seconds() > max_days * 86400:
        raise ValueError(f"window is longer than {max_days} days")
    return {
        "point_id": point_id.replace("'", "''"),
        "start_literal": first.strftime("%Y-%m-%d %H:%M:%S"),
        "end_literal": last.strftime("%Y-%m-%d %H:%M:%S"),
    }


# Points are registered by adding PENDING rows to the control table, which only administrators can write.
known = {r["point_batch_id"] for r in (spark.table(control_table)
         .filter(F.col("source_server") == source_server).filter(F.col("dataset") == "recorded_events")
         .select("point_batch_id").distinct().collect())}

# A ValueError fails this activity, so the Copy activity never runs.
mssparkutils.notebook.exit(json.dumps(validate_window_params(point_id, window_start, window_end, known, max_window_days)))
