import { describe, it, expect } from 'vitest';
import { render, screen } from '@testing-library/react';
import { UploadPage } from './UploadPage';

describe('UploadPage', () => {
  it('renders heading and drop zone', () => {
    render(<UploadPage />);
    expect(screen.getByText('Upload Documents')).toBeInTheDocument();
    expect(screen.getByText(/Drag & drop files here/)).toBeInTheDocument();
    expect(screen.getByText(/Supports PDF/)).toBeInTheDocument();
  });
});
