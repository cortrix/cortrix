import { useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Modal, Input, Select, Button } from '../../components/ui';
import type { UserRecord, UserRole } from '../../types/api';

// Create / Edit user modal (web UI). In create mode all fields are
// editable (email + password + role); in edit mode the password field is
// hidden (auth has no admin password-reset in the 5-endpoint set) and only
// email / display_name / role are patched.

export interface UserFormValues {
  email: string;
  password: string;
  display_name: string;
  role: UserRole;
}

interface UserFormModalProps {
  open: boolean;
  /** Present = edit mode; absent = create mode. */
  user?: UserRecord | null;
  submitting?: boolean;
  onClose: () => void;
  onSubmit: (values: UserFormValues) => Promise<void> | void;
}

const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

export function UserFormModal({ open, user, submitting, onClose, onSubmit }: UserFormModalProps) {
  const { t } = useTranslation();
  const isEdit = !!user;

  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [displayName, setDisplayName] = useState('');
  const [role, setRole] = useState<UserRole>('user');
  const [errors, setErrors] = useState<{ email?: string; password?: string }>({});

  useEffect(() => {
    if (open) {
      setEmail(user?.email ?? '');
      setPassword('');
      setDisplayName(user?.display_name ?? '');
      setRole(user?.role ?? 'user');
      setErrors({});
    }
  }, [open, user]);

  const roleOptions = [
    { value: 'user' as const, label: t('admin.users.roleUser') },
    { value: 'admin' as const, label: t('admin.users.roleAdmin') },
  ];

  const validate = (): boolean => {
    const next: { email?: string; password?: string } = {};
    if (!EMAIL_RE.test(email.trim())) next.email = t('admin.users.invalidEmail');
    if (!isEdit && password.length < 8) next.password = t('admin.users.passwordTooShort');
    setErrors(next);
    return Object.keys(next).length === 0;
  };

  const submit = async () => {
    if (!validate()) return;
    await onSubmit({ email: email.trim(), password, display_name: displayName.trim(), role });
  };

  return (
    <Modal
      open={open}
      onClose={onClose}
      size="sm"
      title={isEdit ? t('admin.users.editTitle') : t('admin.users.createTitle')}
      footer={
        <>
          <Button variant="secondary" onClick={onClose}>
            {t('common.cancel')}
          </Button>
          <Button onClick={() => void submit()} loading={submitting} data-testid="user-form-submit">
            {isEdit ? t('common.save') : t('common.create')}
          </Button>
        </>
      }
    >
      <div className="space-y-4">
        <Input
          id="user-email"
          type="email"
          label={t('admin.users.email')}
          value={email}
          onChange={(e) => setEmail(e.target.value)}
          placeholder="you@example.com"
          error={errors.email}
          data-testid="user-email-input"
        />
        {!isEdit && (
          <Input
            id="user-password"
            type="password"
            label={t('admin.users.password')}
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            placeholder="••••••••"
            error={errors.password}
            data-testid="user-password-input"
          />
        )}
        <Input
          id="user-display-name"
          label={t('admin.users.displayName')}
          value={displayName}
          onChange={(e) => setDisplayName(e.target.value)}
          placeholder={t('admin.users.displayNamePlaceholder')}
        />
        <Select<UserRole> label={t('admin.users.role')} value={role} onChange={setRole} options={roleOptions} />
      </div>
    </Modal>
  );
}
