import type React from 'react';

export function PageHeader({ title, description, actions }: { title: React.ReactNode; description?: React.ReactNode; actions?: React.ReactNode }) {
  return (
    <div className="v2-page-header flex flex-wrap items-start justify-between gap-x-6 gap-y-4">
      <div className="min-w-0 flex-1 basis-64">
        <h1 className="text-2xl font-semibold leading-snug tracking-tight sm:text-[28px]">{title}</h1>
        {description && <p className="mt-2 max-w-3xl text-sm leading-relaxed text-muted-foreground">{description}</p>}
      </div>
      {actions && <div className="flex flex-wrap gap-2 md:pt-1">{actions}</div>}
    </div>
  );
}
