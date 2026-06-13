import { describe, it, expect } from 'vitest';
import {
  isValidFileType,
  isValidFileSize,
  validateFile,
  isValidNamespace,
  validateNamespace,
  validateQuery,
  MAX_FILE_SIZE,
} from './validators';

// Input validation tests (P02a — Upload + Namespace + Search business rules).
// Pure functions, so exhaustive over the accept/reject boundaries.

function fakeFile(name: string, size: number): File {
  // jsdom File: content controls size; pad to the requested byte length.
  return new File([new Uint8Array(Math.max(0, size))], name);
}

describe('isValidFileType', () => {
  it('accepts allowed extensions (case-insensitive)', () => {
    expect(isValidFileType('report.PDF')).toBe(true);
    expect(isValidFileType('notes.md')).toBe(true);
    expect(isValidFileType('photo.jpeg')).toBe(true);
  });
  it('rejects disallowed extensions', () => {
    expect(isValidFileType('script.exe')).toBe(false);
    expect(isValidFileType('archive.zip')).toBe(false);
  });
});

describe('isValidFileSize', () => {
  it('accepts a positive size within the cap', () => {
    expect(isValidFileSize(1024)).toBe(true);
    expect(isValidFileSize(MAX_FILE_SIZE)).toBe(true);
  });
  it('rejects zero / negative / over-cap sizes', () => {
    expect(isValidFileSize(0)).toBe(false);
    expect(isValidFileSize(-1)).toBe(false);
    expect(isValidFileSize(MAX_FILE_SIZE + 1)).toBe(false);
  });
});

describe('validateFile', () => {
  it('passes a valid pdf', () => {
    expect(validateFile(fakeFile('doc.pdf', 2048))).toEqual({ valid: true });
  });
  it('fails an invalid type', () => {
    const r = validateFile(fakeFile('m.exe', 2048));
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.error).toMatch(/Invalid file type/);
  });
  it('fails an empty file', () => {
    const r = validateFile(fakeFile('doc.pdf', 0));
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.error).toMatch(/empty/);
  });
});

describe('isValidNamespace / validateNamespace', () => {
  it('accepts lowercase / digits / underscore, 3-32 chars', () => {
    expect(isValidNamespace('legal_docs')).toBe(true);
    expect(isValidNamespace('ns_01')).toBe(true);
  });
  it('rejects too short / uppercase / illegal chars', () => {
    expect(isValidNamespace('ab')).toBe(false);
    expect(isValidNamespace('Legal')).toBe(false);
    expect(isValidNamespace('has space')).toBe(false);
  });
  it('validateNamespace requires a name', () => {
    const r = validateNamespace('');
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.error).toMatch(/required/);
  });
  it('validateNamespace explains the format rule on bad input', () => {
    const r = validateNamespace('AB');
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.error).toMatch(/3-32/);
  });
  it('validateNamespace passes a good name', () => {
    expect(validateNamespace('legal_docs')).toEqual({ valid: true });
  });
});

describe('validateQuery', () => {
  it('rejects empty / whitespace-only', () => {
    expect(validateQuery('').valid).toBe(false);
    expect(validateQuery('   ').valid).toBe(false);
  });
  it('rejects too-short queries', () => {
    const r = validateQuery('a');
    expect(r.valid).toBe(false);
    if (!r.valid) expect(r.error).toMatch(/too short/);
  });
  it('accepts a real query', () => {
    expect(validateQuery('architecture')).toEqual({ valid: true });
  });
});
