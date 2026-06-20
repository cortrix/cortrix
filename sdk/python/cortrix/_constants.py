"""SDK-wide constants (version, defaults). See P03 design § 3.1."""

SDK_VERSION = "1.0.0"
DEFAULT_BASE_URL = "http://localhost:8420"
DEFAULT_TIMEOUT = 30.0  # seconds
UPLOAD_TIMEOUT = 300.0  # seconds — file upload extends the default timeout
DEFAULT_MAX_RETRIES = 2
API_PREFIX = "/api/v1"
USER_AGENT = f"cortrix-python/{SDK_VERSION}"
