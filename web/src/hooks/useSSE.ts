import { useRef, useCallback } from 'react';
import { API_BASE } from '../utils/constants';
import type { ChatEvent } from '../types/api';

export interface UseSSEOptions {
  onToken?: (text: string) => void;
  onSources?: (sources: unknown[]) => void;
  onDone?: (conversationId: string) => void;
  onError?: (message: string) => void;
}

export function useSSE() {
  const abortControllerRef = useRef<AbortController | null>(null);

  const connect = useCallback(
    async (path: string, body: unknown, options: UseSSEOptions = {}) => {
      if (abortControllerRef.current) {
        abortControllerRef.current.abort();
      }

      const controller = new AbortController();
      abortControllerRef.current = controller;

      try {
        const response = await fetch(`${API_BASE}${path}`, {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
            Accept: 'text/event-stream',
          },
          body: JSON.stringify(body),
          signal: controller.signal,
        });

        if (!response.ok) {
          throw new Error(`HTTP ${response.status}: ${response.statusText}`);
        }

        const reader = response.body?.getReader();
        if (!reader) throw new Error('No response body');

        const decoder = new TextDecoder();
        let buffer = '';

        while (true) {
          const { done, value } = await reader.read();
          if (done) break;

          buffer += decoder.decode(value, { stream: true });
          const lines = buffer.split('\n');
          buffer = lines.pop() || '';

          for (const line of lines) {
            if (line.startsWith('data: ')) {
              const data = line.slice(6);
              if (data === '[DONE]') {
                return;
              }

              try {
                const event = JSON.parse(data) as ChatEvent;

                switch (event.type) {
                  case 'token':
                    options.onToken?.(event.data.text);
                    break;
                  case 'sources':
                    options.onSources?.(event.data.sources);
                    break;
                  case 'done':
                    options.onDone?.(event.data.conversation_id);
                    break;
                  case 'error':
                    options.onError?.(event.data.message);
                    break;
                }
              } catch (parseError) {
                console.error('SSE parse error:', parseError, 'line:', data);
              }
            }
          }
        }
      } catch (error) {
        if (error instanceof Error && error.name === 'AbortError') {
          return;
        }
        options.onError?.(error instanceof Error ? error.message : 'SSE connection failed');
      } finally {
        abortControllerRef.current = null;
      }
    },
    [],
  );

  const disconnect = useCallback(() => {
    if (abortControllerRef.current) {
      abortControllerRef.current.abort();
      abortControllerRef.current = null;
    }
  }, []);

  return { connect, disconnect };
}
