import { describe, it, expect } from 'vitest';
import { render, screen } from '@testing-library/react';
import { SearchPage } from './SearchPage';

describe('SearchPage', () => {
  it('renders heading and search box', () => {
    render(<SearchPage />);
    expect(screen.getByText('Semantic Search')).toBeInTheDocument();
    expect(screen.getByPlaceholderText(/Search across all documents/)).toBeInTheDocument();
    expect(screen.getByText('Search')).toBeInTheDocument();
  });
});
