-- PRIVATE DRAFT ONLY. Never run independently of reviewed Store migration,
-- backup/restore qualification and the complete backend/client integration.
-- Existing17=session issuer;18=session access. Historical MFA17 is inapplicable.
ALTER TABLE users ADD COLUMN authorization_revision INTEGER NOT NULL DEFAULT 1
  CHECK(authorization_revision>=1);
CREATE TRIGGER users_mfa_authorization_revision
AFTER UPDATE OF password_hash,disabled,role,must_change_credentials ON users
WHEN OLD.password_hash!=NEW.password_hash OR OLD.disabled!=NEW.disabled OR
     OLD.role!=NEW.role OR OLD.must_change_credentials!=NEW.must_change_credentials
BEGIN
  UPDATE users SET authorization_revision=authorization_revision+1 WHERE id=NEW.id;
END;
CREATE TABLE account_mfa (
  user_id TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
  generation INTEGER NOT NULL DEFAULT 1 CHECK(generation>=1),
  enabled INTEGER NOT NULL DEFAULT 0 CHECK(enabled IN (0,1)),
  factor_ciphertext TEXT,
  last_accepted_step INTEGER NOT NULL DEFAULT -1 CHECK(last_accepted_step>=-1),
  changed_at INTEGER NOT NULL CHECK(changed_at>=0),
  CHECK((enabled=0 AND factor_ciphertext IS NULL) OR
        (enabled=1 AND factor_ciphertext IS NOT NULL))
);
CREATE TABLE mfa_recovery_codes (
  user_id TEXT NOT NULL REFERENCES account_mfa(user_id) ON DELETE CASCADE,
  generation INTEGER NOT NULL CHECK(generation>=1),
  code_hash TEXT NOT NULL CHECK(length(code_hash)=64),
  consumed_at INTEGER CHECK(consumed_at IS NULL OR consumed_at>=0),
  PRIMARY KEY(user_id,generation,code_hash)
);
CREATE TABLE mfa_failure_budgets (
  user_id TEXT PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
  window_started_at INTEGER NOT NULL CHECK(window_started_at>=0),
  failures INTEGER NOT NULL DEFAULT 0 CHECK(failures BETWEEN 0 AND 5),
  blocked_until INTEGER NOT NULL DEFAULT 0 CHECK(blocked_until>=0),
  last_observed_at INTEGER NOT NULL CHECK(last_observed_at>=0)
);
-- Existing users have no row and remain OFF. Enabling is explicit verified
-- self-enrollment, never this migration. PRAGMA user_version=19 belongs only
-- in Store's encompassing reviewed BEGIN IMMEDIATE/COMMIT transaction.
