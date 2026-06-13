import type { SearchResult } from './api';

export interface UploadFile {
  id: string;
  file: File;
  doc_id?: number;
  progress: number;
  status: 'pending' | 'uploading' | 'processing' | 'ready' | 'error';
  error?: string;
}

export interface ChatMessage {
  id: string;
  role: 'user' | 'assistant';
  content: string;
  sources?: SearchResult[];
  timestamp: string;
}
