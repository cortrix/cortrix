import { useEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import type { ChatMessage } from '../../types/state';
import { Message } from './Message';

interface MessageListProps {
  messages: ChatMessage[];
  isStreaming: boolean;
}

export function MessageList({ messages, isStreaming }: MessageListProps) {
  const endRef = useRef<HTMLDivElement>(null);
  const { t } = useTranslation();

  useEffect(() => {
    endRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [messages, isStreaming]);

  if (messages.length === 0) {
    return (
      <div className="flex-1 flex items-center justify-center text-muted">
        <div className="text-center">
          <svg className="w-16 h-16 mx-auto mb-4 opacity-30" fill="none" stroke="currentColor" viewBox="0 0 24 24">
            <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={1} d="M8 12h.01M12 12h.01M16 12h.01M21 12c0 4.418-4.03 8-9 8a9.863 9.863 0 01-4.255-.949L3 20l1.395-3.72C3.512 15.042 3 13.574 3 12c0-4.418 4.03-8 9-8s9 3.582 9 8z" />
          </svg>
          <p className="text-lg font-medium">{t('chat.emptyTitle')}</p>
          <p className="text-sm mt-1">{t('chat.emptySubtitle')}</p>
        </div>
      </div>
    );
  }

  return (
    <div className="flex-1 overflow-y-auto p-6 space-y-6 bg-bg">
      {messages.map((m, i) => {
        const isLastAssistant = isStreaming && m.role === 'assistant' && i === messages.length - 1;
        return <Message key={m.id} message={m} isStreaming={isLastAssistant} />;
      })}
      <div ref={endRef} />
    </div>
  );
}
