-- Lakehouse tables for the Fabric path. Run once, before the first pipeline run. Status: Not run.
-- Timestamps in the control table are canonical UTC text, as in control_record.schema.json.

CREATE TABLE IF NOT EXISTS control_extraction (
  source_server STRING NOT NULL, dataset STRING NOT NULL, point_batch_id STRING NOT NULL,
  window_start_utc STRING NOT NULL, window_end_utc STRING NOT NULL,
  run_id STRING NOT NULL, attempt INT NOT NULL, status STRING NOT NULL,
  expected_count BIGINT, observed_count BIGINT, checksum STRING, error STRING,
  last_committed_watermark_utc STRING
) USING DELTA;

-- Grain: one recorded event. Each commit replaces one point and window, so no durable event key is assumed.
-- (SourceServer, PointId, EventUtcRaw, EventSeq) is unique within a commit only.
CREATE TABLE IF NOT EXISTS curated_recorded_events (
  SourceBackend STRING NOT NULL, SourceServer STRING NOT NULL, PointId STRING NOT NULL, TagName STRING NOT NULL,
  EventUtc TIMESTAMP NOT NULL, EventUtcRaw STRING NOT NULL, EventSeq INT NOT NULL, RetrievalMode STRING NOT NULL,
  ValueKind STRING NOT NULL, NumericValue DOUBLE, TextValue STRING, DigitalCode INT, StateText STRING, EngUnits STRING,
  IsGood BOOLEAN NOT NULL, IsQuestionable BOOLEAN NOT NULL, IsSubstituted BOOLEAN NOT NULL, IsAnnotated BOOLEAN NOT NULL,
  ResultScope STRING NOT NULL, window_start_utc TIMESTAMP NOT NULL, window_end_utc TIMESTAMP NOT NULL,
  run_id STRING NOT NULL, attempt INT NOT NULL
) USING DELTA PARTITIONED BY (SourceServer);

-- Grain: one point per source. Key: (SourceServer, PointId). A rename changes TagName only.
CREATE TABLE IF NOT EXISTS curated_points (
  SourceBackend STRING NOT NULL, SourceServer STRING NOT NULL, PointId STRING NOT NULL, TagName STRING NOT NULL,
  Descriptor STRING, PointClass STRING, PointType STRING, EngUnits STRING, DigitalSetName STRING, IsStep BOOLEAN,
  IsActive BOOLEAN NOT NULL, last_seen_run_id STRING NOT NULL
) USING DELTA;

-- The next two stay empty until an AF and an Event Frames backend exist. They fix the grain now, so later
-- work doesn't change the model; no driver table or notebook writes to them yet.

-- Grain: one AF attribute. Several attributes may reference one PI point, so facts stay keyed on PointId.
CREATE TABLE IF NOT EXISTS curated_af_attributes (
  SourceServer STRING NOT NULL, AttributeId STRING NOT NULL, AttributePath STRING NOT NULL, ElementPath STRING,
  PointId STRING, EngUnits STRING, DesiredUnits STRING, IsActive BOOLEAN NOT NULL
) USING DELTA;

-- Grain: one event frame. EndUtc is NULL while the frame is open, and it may span extraction windows.
CREATE TABLE IF NOT EXISTS curated_event_frames (
  SourceServer STRING NOT NULL, FrameId STRING NOT NULL, ParentFrameId STRING, Name STRING NOT NULL,
  TemplateName STRING, ElementPath STRING, StartUtc TIMESTAMP NOT NULL, EndUtc TIMESTAMP
) USING DELTA;
