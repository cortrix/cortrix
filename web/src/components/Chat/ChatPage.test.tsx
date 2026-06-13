import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen } from '@testing-library/react';
import { ChatPage } from './ChatPage';

// Mock useAppStore to control llm_enabled
vi.mock('../../store/useAppStore', () => ({
  useAppStore: vi.fn(() => ({
    systemStatus: { llm_enabled: true, llm_provider: 'GLM-4' },
  })),
}));

import { useAppStore } from '../../store/useAppStore';
const mockUseAppStore = vi.mocked(useAppStore);

describe('ChatPage', () => {
  beforeEach(() => {
    mockUseAppStore.mockReturnValue({
      systemStatus: { llm_enabled: true, llm_provider: 'GLM-4' },
    } as any);
  });

  it('renders heading and input', () => {
    render(<ChatPage />);
    // i18n key: chat.title = "Cortrix Agent"
    expect(screen.getByRole('heading', { name: /Cortrix Agent/i })).toBeInTheDocument();
    expect(screen.getByPlaceholderText(/Ask anything/i)).toBeInTheDocument();
  });

  it('shows New Session button', () => {
    render(<ChatPage />);
    expect(screen.getByText(/New Session/i)).toBeInTheDocument();
  });

  it('does NOT show retrieval banner when LLM is enabled', () => {
    render(<ChatPage />);
    expect(screen.queryByText(/Retrieval Mode/i)).not.toBeInTheDocument();
  });

  it('shows retrieval mode banner when LLM is disabled', () => {
    mockUseAppStore.mockReturnValue({
      systemStatus: { llm_enabled: false, llm_provider: '' },
    } as any);

    render(<ChatPage />);
    expect(screen.getByText(/Retrieval Mode/i)).toBeInTheDocument();
  });
});
