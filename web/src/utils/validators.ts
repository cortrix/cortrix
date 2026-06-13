export const ALLOWED_FILE_EXTENSIONS = [
  '.pdf',
  '.txt',
  '.md',
  '.docx',
  '.doc',
  '.png',
  '.jpg',
  '.jpeg',
] as const;

export const MAX_FILE_SIZE = 100 * 1024 * 1024;

export function isValidFileType(filename: string): boolean {
  const ext = filename.toLowerCase().slice(filename.lastIndexOf('.'));
  return ALLOWED_FILE_EXTENSIONS.some((allowed) => ext === allowed);
}

export function isValidFileSize(size: number): boolean {
  return size > 0 && size <= MAX_FILE_SIZE;
}

export function validateFile(
  file: File,
): { valid: true } | { valid: false; error: string } {
  if (!isValidFileType(file.name)) {
    return {
      valid: false,
      error: `Invalid file type. Allowed: ${ALLOWED_FILE_EXTENSIONS.join(', ')}`,
    };
  }

  if (!isValidFileSize(file.size)) {
    if (file.size === 0) {
      return { valid: false, error: 'File is empty' };
    }
    return {
      valid: false,
      error: `File too large. Max size: ${MAX_FILE_SIZE / 1024 / 1024}MB`,
    };
  }

  return { valid: true };
}

export function isValidNamespace(name: string): boolean {
  return /^[a-z0-9_]{3,32}$/.test(name);
}

export function validateNamespace(
  name: string,
): { valid: true } | { valid: false; error: string } {
  if (!name) {
    return { valid: false, error: 'Namespace name is required' };
  }

  if (!isValidNamespace(name)) {
    return {
      valid: false,
      error: 'Namespace must be 3-32 characters, lowercase letters, numbers, and underscores only',
    };
  }

  return { valid: true };
}

export function validateQuery(query: string): { valid: true } | { valid: false; error: string } {
  if (!query || query.trim().length === 0) {
    return { valid: false, error: 'Query cannot be empty' };
  }

  if (query.trim().length < 2) {
    return { valid: false, error: 'Query too short (minimum 2 characters)' };
  }

  return { valid: true };
}
