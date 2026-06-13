export const API_BASE = '';
export const POLL_INTERVAL_MS = 2000;
export const SYSTEM_POLL_MS = 5000;
export const MAX_FILE_SIZE = 100 * 1024 * 1024; // 100MB

export const ACCEPTED_TYPES: Record<string, string[]> = {
  'application/pdf': ['.pdf'],
  'text/plain': ['.txt'],
  'text/markdown': ['.md'],
  'application/vnd.openxmlformats-officedocument.wordprocessingml.document': ['.docx'],
  'image/png': ['.png'],
  'image/jpeg': ['.jpg', '.jpeg'],
};
