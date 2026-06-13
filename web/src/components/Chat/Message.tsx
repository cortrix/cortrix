import { useTranslation } from 'react-i18next';
import type { ChatMessage } from '../../types/state';
import { SourceCitation } from './SourceCitation';
import { SafeMarkdown } from '../Common/SafeMarkdown';

interface MessageProps {
  message: ChatMessage;
  isStreaming?: boolean;
}

export function Message({ message, isStreaming }: MessageProps) {
  const isUser = message.role === 'user';
  const { t } = useTranslation();

  if (isUser) {
    return (
      <div className="flex justify-end">
        <div className="max-w-[70%] min-w-0 overflow-hidden px-4 py-3 rounded-2xl rounded-br-md shadow-sm bg-cortrix-600 text-white dark:bg-cortrix-800">
          <p className="text-sm break-words">{message.content}</p>
        </div>
      </div>
    );
  }

  return (
    <div className="flex justify-start">
      <div className="max-w-[75%] min-w-0 overflow-hidden px-5 py-4 rounded-2xl rounded-bl-md shadow-sm bg-surface border border-line ">
        <div className="text-sm leading-relaxed text-txt prose prose-sm dark:prose-invert max-w-none break-words prose-pre:overflow-x-auto prose-code:break-all">
          <SafeMarkdown content={message.content} />
          {isStreaming && message.content.length > 0 && (
            <span className="inline-block w-2 h-4 rounded-sm animate-pulse ml-0.5 bg-cortrix-500 dark:bg-cortrix-400" />
          )}
        </div>
        {isStreaming && !message.content && (
          <div className="flex items-center gap-2 text-xs text-cortrix-500 dark:text-cortrix-400">
            <div className="flex gap-1">
              <span className="w-1.5 h-1.5 rounded-full bg-cortrix-500 dark:bg-cortrix-400 animate-pulse" />
              <span className="w-1.5 h-1.5 rounded-full bg-cortrix-500 dark:bg-cortrix-400 animate-pulse [animation-delay:0.2s]" />
              <span className="w-1.5 h-1.5 rounded-full bg-cortrix-500 dark:bg-cortrix-400 animate-pulse [animation-delay:0.4s]" />
            </div>
            {t('chat.thinking')}
          </div>
        )}
        {message.sources && <SourceCitation sources={message.sources} />}
      </div>
    </div>
  );
}
