import { defineConfig } from "@playwright/test";
import { harness } from "./playwright.config";

export default defineConfig(harness.gpu);
