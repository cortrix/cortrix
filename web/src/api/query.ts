import { post } from './client';
import type { QueryRequest, QueryResponse } from '../types/api';

export async function query(request: QueryRequest): Promise<QueryResponse> {
  return post<QueryResponse>('/api/v1/query', request);
}
