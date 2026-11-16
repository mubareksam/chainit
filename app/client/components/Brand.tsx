import React from 'react';

/**
 * The ChainIt mark — the exact chain-link artwork used by the Android launcher icon
 * (android/app/src/main/res/drawable/ic_launcher.xml) so both apps share one identity.
 */
export const ChainItLogo: React.FC<{ size?: number; className?: string }> = ({
  size = 36,
  className = '',
}) => (
  <svg
    width={size}
    height={size}
    viewBox="0 0 108 108"
    fill="none"
    xmlns="http://www.w3.org/2000/svg"
    className={className}
    role="img"
    aria-label="ChainIt"
  >
    <rect width="108" height="108" rx="24" fill="#090D16" />
    <path
      d="M42 36h24c9.9 0 18 8.1 18 18s-8.1 18 -18 18h-10v-8h10c5.5 0 10-4.5 10-10s-4.5-10 -10-10h-24c-5.5 0 -10 4.5 -10 10v2h-8v-2c0-9.9 8.1-18 18-18z"
      fill="#10B981"
    />
    <path
      d="M66 72h-24c-9.9 0 -18-8.1 -18-18s8.1-18 18-18h10v8h-10c-5.5 0 -10 4.5 -10 10s4.5 10 10 10h24c5.5 0 10-4.5 10-10v-2h8v2c0 9.9-8.1 18-18 18z"
      fill="#06B6D4"
    />
  </svg>
);

/** Compact square badge used in headers, tray-like strips and the mobile companion. */
export const LogoBadge: React.FC<{ size?: number }> = ({ size = 36 }) => (
  <span className="ci-logo-mark" style={{ width: size, height: size }}>
    <ChainItLogo size={Math.round(size * 0.86)} />
  </span>
);
