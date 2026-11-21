export function Brand({ compact = false }: { compact?: boolean }) {
  return (
    <span className="flex items-center gap-2 overflow-hidden">
      <img src="./favicon.svg" alt="" className="h-7 w-7 shrink-0" />
      {!compact && <span className="v2-wordmark text-foreground">Cat<span className="text-primary">Official</span></span>}
    </span>
  );
}
