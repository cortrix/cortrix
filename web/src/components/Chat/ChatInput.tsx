import { useState, type KeyboardEvent } from 'react';
import { PaperAirplaneIcon, StopIcon } from '@heroicons/react/24/outline';
import { useTranslation } from 'react-i18next';

interface ChatInputProps {
  onSend: (text: string) => void;
  onStop: () => void;
  disabled: boolean;
  isStreaming: boolean;
}

export function ChatInput({ onSend, onStop, disabled, isStreaming }: ChatInputProps) {
  const [text, setText] = useState('');
  const { t } = useTranslation();

  const handleSend = () => {
    if (!text.trim() || disabled) return;
    onSend(text.trim());
    setText('');
  };

  const handleKeyDown = (e: KeyboardEvent) => {
    if (e.key === 'Enter' && !e.shiftKey) {
      e.preventDefault();
      handleSend();
    }
  };

  return (
    <div className="px-6 py-4 shrink-0 border-t border-line bg-surface ">
      <div className="flex gap-3 max-w-4xl mx-auto">
        <input
          type="text"
          value={text}
          onChange={(e) => setText(e.target.value)}
          onKeyDown={handleKeyDown}
          placeholder={t('chat.placeholder')}
          disabled={disabled}
          aria-label={t('chat.placeholder')}
          data-testid="chat-input"
          className="flex-1 px-4 py-3 rounded-xl text-sm outline-none border border-line focus:ring-2 focus:ring-magma/20 focus:border-magma bg-surface text-txt placeholder:text-muted disabled:opacity-50"
        />
        {isStreaming ? (
          <button
            onClick={onStop}
            aria-label={t('chat.stop')}
            data-testid="chat-stop-btn"
            className="px-5 py-3 rounded-xl text-sm font-medium flex items-center gap-2 transition-colors bg-red-500 text-white hover:bg-red-600"
          >
            <StopIcon className="w-4 h-4" aria-hidden="true" />
            {t('chat.stop')}
          </button>
        ) : (
          <button
            onClick={handleSend}
            disabled={!text.trim()}
            aria-label={t('chat.send')}
            data-testid="chat-send-btn"
            className="px-5 py-3 rounded-xl text-sm font-medium flex items-center gap-2 transition-colors bg-cortrix-600 text-white hover:bg-cortrix-700 disabled:opacity-50 disabled:cursor-not-allowed"
          >
            <PaperAirplaneIcon className="w-4 h-4" aria-hidden="true" />
            {t('chat.send')}
          </button>
        )}
      </div>
      <p className="text-xs mt-2 text-center text-muted">
        {t('chat.inputHint')}
      </p>
    </div>
  );
}
