import { strings } from "./strings.js";
export type Language = "ko" | "en";
export type StringKey = keyof typeof strings.en;
export function languageOf(appLanguage: string): Language { return appLanguage === "ko" ? "ko" : "en"; }
export function translator(appLanguage: string): (key: StringKey) => string {
  const locale = strings[languageOf(appLanguage)];
  return key => locale[key];
}
